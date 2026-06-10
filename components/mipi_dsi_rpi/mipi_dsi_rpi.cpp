#ifdef USE_ESP32_VARIANT_ESP32P4
#include "mipi_dsi_rpi.h"
#include "esphome/core/helpers.h"

// Low-level DSI HAL access (ESP-IDF). Used to reach the DSI host for the overrides this
// panel requires (non-burst sync-pulse mode, continuous HS clock, frame/cmd ACK disable)
// and to send TC358762 config as DSI Generic Long Writes. Ported from embenix.
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_types.h"
#include "hal/mipi_dsi_ll.h"

namespace esphome::mipi_dsi_rpi {

// Mirror the private esp_lcd DSI bus struct so we can reach the HAL context from the opaque
// bus handle. Matches esp_lcd/dsi/mipi_dsi_priv.h. Update if the IDF layout changes.
typedef struct {
  int bus_id;
  mipi_dsi_hal_context_t hal;
} rpi_dsi_bus_priv_t;

// ---- ATTINY88 control MCU (I2C 0x45) registers (rpi-panel-attiny-regulator.c) ----
static const uint8_t REG_PORTA = 0x81;
static const uint8_t REG_PORTB = 0x82;
static const uint8_t REG_PORTC = 0x83;
static const uint8_t REG_PWM = 0x86;
static const uint8_t REG_ID = 0x80;
static const uint8_t REG_ADDR_L = 0x8c;
static const uint8_t REG_ADDR_H = 0x8d;
static const uint8_t REG_WRITE_DATA_H = 0x90;
static const uint8_t REG_WRITE_DATA_L = 0x91;
// PORTA / PORTB / PORTC bit fields
static const uint8_t PA_LCD_LR = (1 << 2);
static const uint8_t PB_LCD_MAIN = (1 << 7);
static const uint8_t PC_LED_EN = (1 << 0);
static const uint8_t PC_RST_TP_N = (1 << 1);
static const uint8_t PC_RST_LCD_N = (1 << 2);
static const uint8_t PC_RST_BRIDGE_N = (1 << 3);

// ---- TC358762 DSI->DPI bridge registers (tc358762.c) ----
static const uint16_t TC_DSI_LANEENABLE = 0x0210;
static const uint16_t TC_PPI_D0S_CLRSIPOCOUNT = 0x0164;
static const uint16_t TC_PPI_D1S_CLRSIPOCOUNT = 0x0168;
static const uint16_t TC_PPI_D0S_ATMR = 0x0144;
static const uint16_t TC_PPI_D1S_ATMR = 0x0148;
static const uint16_t TC_PPI_LPTXTIMECNT = 0x0114;
static const uint16_t TC_SPICMR = 0x0450;
static const uint16_t TC_LCDCTRL = 0x0420;
static const uint16_t TC_SYSCTRL = 0x0464;
static const uint16_t TC_LCD_HS_HBP = 0x0424;
static const uint16_t TC_LCD_HDISP_HFP = 0x0428;
static const uint16_t TC_LCD_VS_VBP = 0x042c;
static const uint16_t TC_LCD_VDISP_VFP = 0x0430;
static const uint16_t TC_PPI_STARTPPI = 0x0104;
static const uint16_t TC_DSI_STARTDSI = 0x0204;

static bool notify_refresh_ready(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
  SemaphoreHandle_t sem = static_cast<SemaphoreHandle_t>(user_ctx);
  BaseType_t need_yield = pdFALSE;
  xSemaphoreGiveFromISR(sem, &need_yield);
  return (need_yield == pdTRUE);
}

void MipiDsiRpi::smark_failed_(const char *message, esp_err_t err) {
  ESP_LOGE(TAG, "%s: %s", message, esp_err_to_name(err));
  this->mark_failed();
}

// ----------------------- ATTINY88 helpers (ESPHome I2C) -----------------------
esp_err_t MipiDsiRpi::attiny_write_register_(uint8_t reg, uint8_t val) {
  return this->write_byte(reg, val) ? ESP_OK : ESP_FAIL;
}
esp_err_t MipiDsiRpi::attiny_read_register_(uint8_t reg, uint8_t *val) {
  return this->read_byte(reg, val) ? ESP_OK : ESP_FAIL;
}

// Mirrors attiny_lcd_power_enable(): assert resets, set scan dir, main regulator on,
// LED on (bridge stays in reset). 80ms settle.
esp_err_t MipiDsiRpi::lcd_power_on_sequence_() {
  this->lcd_enabled_ = false;
  if (this->attiny_write_register_(REG_PORTC, 0x00) != ESP_OK)
    return ESP_FAIL;
  delay(10);
  this->attiny_write_register_(REG_PORTA, PA_LCD_LR);
  delay(10);
  this->attiny_write_register_(REG_PORTB, PB_LCD_MAIN);
  delay(10);
  this->attiny_write_register_(REG_PORTC, PC_LED_EN);
  delay(80);
  this->lcd_enabled_ = true;
  ESP_LOGI(TAG, "ATTINY88 power-on complete (bridge held in reset)");
  return ESP_OK;
}

// Release TC358762 reset and wake the bridge core via the ATTINY88 SPI proxy
// (writes TC358762 SYSPMCTRL 0x047C = 0x0000).
esp_err_t MipiDsiRpi::release_bridge_reset_and_wake_() {
  this->attiny_write_register_(REG_PORTC, PC_LED_EN | PC_RST_LCD_N | PC_RST_BRIDGE_N);
  delay(10);
  this->attiny_write_register_(REG_ADDR_H, 0x04);
  delay(8);
  this->attiny_write_register_(REG_ADDR_L, 0x7c);
  delay(8);
  this->attiny_write_register_(REG_WRITE_DATA_H, 0x00);
  delay(8);
  this->attiny_write_register_(REG_WRITE_DATA_L, 0x00);
  delay(100);
  ESP_LOGI(TAG, "TC358762 reset released + SYSPMCTRL wake sent");
  return ESP_OK;
}

// Release the touch controller reset so the ft5x06 component can probe it.
esp_err_t MipiDsiRpi::release_touch_reset_() {
  this->attiny_write_register_(REG_PORTC, PC_LED_EN | PC_RST_TP_N | PC_RST_LCD_N | PC_RST_BRIDGE_N);
  delay(10);
  return ESP_OK;
}

void MipiDsiRpi::set_backlight(uint8_t brightness) {
  if (!this->lcd_enabled_)
    return;
  this->attiny_write_register_(REG_PWM, brightness);
}

// ----------------------- TC358762 bridge config (DSI Generic Long Write) -----------------------
void MipiDsiRpi::tc358762_reg_write_(uint16_t reg, uint32_t val) {
  auto *priv = reinterpret_cast<rpi_dsi_bus_priv_t *>(this->bus_handle_);
  uint8_t payload[6] = {
      static_cast<uint8_t>(reg & 0xFF),        static_cast<uint8_t>((reg >> 8) & 0xFF),
      static_cast<uint8_t>(val & 0xFF),        static_cast<uint8_t>((val >> 8) & 0xFF),
      static_cast<uint8_t>((val >> 16) & 0xFF), static_cast<uint8_t>((val >> 24) & 0xFF),
  };
  mipi_dsi_hal_host_gen_write_long_packet(&priv->hal, 0, MIPI_DSI_DT_GENERIC_LONG_WRITE, payload, sizeof(payload));
}

esp_err_t MipiDsiRpi::tc358762_bridge_init_() {
  ESP_LOGI(TAG, "Configuring TC358762 bridge");
  this->tc358762_reg_write_(TC_DSI_LANEENABLE, (1 << 0) | (1 << 1));  // CLEN + D0EN (1 data lane)
  this->tc358762_reg_write_(TC_PPI_D0S_CLRSIPOCOUNT, 0x05);
  this->tc358762_reg_write_(TC_PPI_D1S_CLRSIPOCOUNT, 0x05);
  this->tc358762_reg_write_(TC_PPI_D0S_ATMR, 0x00);
  this->tc358762_reg_write_(TC_PPI_D1S_ATMR, 0x00);
  this->tc358762_reg_write_(TC_PPI_LPTXTIMECNT, 0x03);
  this->tc358762_reg_write_(TC_SPICMR, 0x00);
  this->tc358762_reg_write_(TC_LCDCTRL, 0x00100150);
  this->tc358762_reg_write_(TC_SYSCTRL, 0x040f);
  this->tc358762_reg_write_(TC_LCD_HS_HBP, (this->hsync_back_porch_ << 16) | this->hsync_pulse_width_);
  this->tc358762_reg_write_(TC_LCD_HDISP_HFP, (this->hsync_front_porch_ << 16) | this->width_);
  this->tc358762_reg_write_(TC_LCD_VS_VBP, (this->vsync_back_porch_ << 16) | this->vsync_pulse_width_);
  this->tc358762_reg_write_(TC_LCD_VDISP_VFP, (this->vsync_front_porch_ << 16) | this->height_);
  delay(100);
  this->tc358762_reg_write_(TC_PPI_STARTPPI, 0x01);
  this->tc358762_reg_write_(TC_DSI_STARTDSI, 0x01);
  delay(100);
  ESP_LOGI(TAG, "TC358762 bridge configured");
  return ESP_OK;
}

// ----------------------------------- setup -----------------------------------
void MipiDsiRpi::setup() {
  ESP_LOGCONFIG(TAG, "Setting up RPi 7\" V1 / Hosyond MIPI-DSI panel");

  // NOTE: the DSI PHY LDO (channel 3 @ 2500mV on this board) must be powered by an
  // `esp_ldo:` entry in the YAML. ESPHome's stock mipi_dsi relies on the same mechanism.

  // Read ATTINY88 firmware ID (sanity log).
  uint8_t fw_id = 0;
  if (this->attiny_read_register_(REG_ID, &fw_id) == ESP_OK) {
    ESP_LOGI(TAG, "ATTINY88 firmware ID: 0x%02X", fw_id);
  } else {
    ESP_LOGW(TAG, "Could not read ATTINY88 firmware ID (I2C 0x45)");
  }

  if (this->lcd_power_on_sequence_() != ESP_OK) {
    this->smark_failed_("ATTINY88 power-on failed", ESP_FAIL);
    return;
  }

  // 1) DSI bus (1 lane, 600 Mbps).
  esp_lcd_dsi_bus_config_t bus_config = {
      .bus_id = 0,
      .num_data_lanes = this->lanes_,
      .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
      .lane_bit_rate_mbps = static_cast<uint32_t>(this->lane_bit_rate_),
  };
  esp_err_t err = esp_lcd_new_dsi_bus(&bus_config, &this->bus_handle_);
  if (err != ESP_OK)
    return this->smark_failed_("esp_lcd_new_dsi_bus failed", err);

  // 2) Create a DBI IO purely to latch LP-mode for Generic Long Writes, then delete it
  //    (the LP setting persists in the DSI controller).
  esp_lcd_panel_io_handle_t dbi_io = nullptr;
  esp_lcd_dbi_io_config_t dbi_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
  err = esp_lcd_new_panel_io_dbi(this->bus_handle_, &dbi_cfg, &dbi_io);
  if (err != ESP_OK)
    return this->smark_failed_("esp_lcd_new_panel_io_dbi failed", err);
  esp_lcd_panel_io_del(dbi_io);

  // 3) DPI panel (RGB888, num_fbs=1, panel timings). disable_lp=0 keeps LP command windows
  //    open during blanking so the bridge config packets can be inserted.
  esp_lcd_dpi_panel_config_t dpi_config = {
      .virtual_channel = 0,
      .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
      .dpi_clock_freq_mhz = this->pclk_frequency_,
      .in_color_format = LCD_COLOR_FMT_RGB888,
      .out_color_format = LCD_COLOR_FMT_RGB888,
      .num_fbs = 1,
      .video_timing =
          {
              .h_size = static_cast<uint32_t>(this->width_),
              .v_size = static_cast<uint32_t>(this->height_),
              .hsync_pulse_width = this->hsync_pulse_width_,
              .hsync_back_porch = this->hsync_back_porch_,
              .hsync_front_porch = this->hsync_front_porch_,
              .vsync_pulse_width = this->vsync_pulse_width_,
              .vsync_back_porch = this->vsync_back_porch_,
              .vsync_front_porch = this->vsync_front_porch_,
          },
      .flags =
          {
              .disable_lp = 0,
          },
  };
  err = esp_lcd_new_panel_dpi(this->bus_handle_, &dpi_config, &this->handle_);
  if (err != ESP_OK)
    return this->smark_failed_("esp_lcd_new_panel_dpi failed", err);

  // 4) Override video mode to NON-BURST sync-pulses + disable frame ACK (before panel_init).
  auto *priv = reinterpret_cast<rpi_dsi_bus_priv_t *>(this->bus_handle_);
  mipi_dsi_host_ll_dpi_set_video_burst_type(priv->hal.host, MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
  mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, false);

  err = esp_lcd_panel_init(this->handle_);
  if (err != ESP_OK)
    return this->smark_failed_("esp_lcd_panel_init failed", err);

  // 5) Force continuous HS clock (TC358762 FLL needs a stable reference) + disable cmd ACK.
  mipi_dsi_host_ll_set_clock_lane_state(priv->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
  mipi_dsi_host_ll_enable_cmd_ack(priv->hal.host, false);

  // 6) Release bridge reset + configure TC358762 (video running, HS clock active).
  this->release_bridge_reset_and_wake_();
  this->tc358762_bridge_init_();

  // 7) Release touch reset so the ft5x06 component can probe FT5406.
  this->release_touch_reset_();

  // 8) Backlight full on.
  this->set_backlight(255);

  // 9) Frame-done semaphore (write_to_display_ waits on this after each draw_bitmap).
  this->io_lock_ = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_color_trans_done = notify_refresh_ready};
  err = esp_lcd_dpi_panel_register_event_callbacks(this->handle_, &cbs, this->io_lock_);
  if (err != ESP_OK)
    return this->smark_failed_("register_event_callbacks failed", err);

  ESP_LOGCONFIG(TAG, "RPi 7\" V1 / Hosyond panel setup complete");
}

// ----------------------- rendering (verbatim from ESPHome stock mipi_dsi) -----------------------
void MipiDsiRpi::update() {
  if (this->auto_clear_enabled_)
    this->clear();
  if (this->show_test_card_) {
    this->test_card();
  } else if (this->page_ != nullptr) {
    this->page_->get_writer()(*this);
  } else if (this->writer_.has_value()) {
    (*this->writer_)(*this);
  } else {
    this->stop_poller();
  }
  if (this->buffer_ == nullptr || this->x_low_ > this->x_high_ || this->y_low_ > this->y_high_)
    return;
  int w = this->x_high_ - this->x_low_ + 1;
  int h = this->y_high_ - this->y_low_ + 1;
  this->write_to_display_(this->x_low_, this->y_low_, w, h, this->buffer_, this->x_low_, this->y_low_,
                          this->width_ - w - this->x_low_);
  this->x_low_ = this->width_;
  this->y_low_ = this->height_;
  this->x_high_ = 0;
  this->y_high_ = 0;
}

void MipiDsiRpi::draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                                display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) {
  if (w <= 0 || h <= 0)
    return;
  if (bitness != this->color_depth_) {
    display::Display::draw_pixels_at(x_start, y_start, w, h, ptr, order, bitness, big_endian, x_offset, y_offset, x_pad);
    return;
  }
  this->write_to_display_(x_start, y_start, w, h, ptr, x_offset, y_offset, x_pad);
}

void MipiDsiRpi::write_to_display_(int x_start, int y_start, int w, int h, const uint8_t *ptr, int x_offset,
                                   int y_offset, int x_pad) {
  esp_err_t err = ESP_OK;
  auto bytes_per_pixel = 3 - this->color_depth_;
  auto stride = (x_offset + w + x_pad) * bytes_per_pixel;
  ptr += y_offset * stride + x_offset * bytes_per_pixel;
  if (x_offset == 0 && x_pad == 0) {
    err = esp_lcd_panel_draw_bitmap(this->handle_, x_start, y_start, x_start + w, y_start + h, ptr);
    xSemaphoreTake(this->io_lock_, portMAX_DELAY);
  } else {
    for (int y = 0; y != h; y++) {
      err = esp_lcd_panel_draw_bitmap(this->handle_, x_start, y + y_start, x_start + w, y + y_start + 1, ptr);
      if (err != ESP_OK)
        break;
      ptr += stride;
      xSemaphoreTake(this->io_lock_, portMAX_DELAY);
    }
  }
  if (err != ESP_OK)
    ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
}

bool MipiDsiRpi::check_buffer_() {
  if (this->is_failed())
    return false;
  if (this->buffer_ != nullptr)
    return true;
  auto bytes_per_pixel = 3 - this->color_depth_;
  RAMAllocator<uint8_t> allocator;
  this->buffer_ = allocator.allocate(this->height_ * this->width_ * bytes_per_pixel);
  if (this->buffer_ == nullptr) {
    this->mark_failed();
    return false;
  }
  return true;
}

void MipiDsiRpi::draw_pixel_at(int x, int y, Color color) {
  if (!this->get_clipping().inside(x, y))
    return;
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_0_DEGREES:
      break;
    case display::DISPLAY_ROTATION_90_DEGREES:
      std::swap(x, y);
      x = this->width_ - x - 1;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      x = this->width_ - x - 1;
      y = this->height_ - y - 1;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      std::swap(x, y);
      y = this->height_ - y - 1;
      break;
  }
  if (x >= this->get_width_internal() || x < 0 || y >= this->get_height_internal() || y < 0)
    return;
  if (!this->check_buffer_())
    return;
  size_t pos = (y * this->width_) + x;
  switch (this->color_depth_) {
    case display::COLOR_BITNESS_565: {
      auto *ptr_16 = reinterpret_cast<uint16_t *>(this->buffer_);
      uint8_t hi_byte = static_cast<uint8_t>(color.r & 0xF8) | (color.g >> 5);
      uint8_t lo_byte = static_cast<uint8_t>((color.g & 0x1C) << 3) | (color.b >> 3);
      uint16_t new_color = lo_byte | (hi_byte << 8);
      if (ptr_16[pos] == new_color)
        return;
      ptr_16[pos] = new_color;
      break;
    }
    case display::COLOR_BITNESS_888:
      if (this->color_mode_ == display::COLOR_ORDER_BGR) {
        this->buffer_[pos * 3] = color.b;
        this->buffer_[pos * 3 + 1] = color.g;
        this->buffer_[pos * 3 + 2] = color.r;
      } else {
        this->buffer_[pos * 3] = color.r;
        this->buffer_[pos * 3 + 1] = color.g;
        this->buffer_[pos * 3 + 2] = color.b;
      }
      break;
    case display::COLOR_BITNESS_332:
      break;
  }
  if (x < this->x_low_)
    this->x_low_ = x;
  if (y < this->y_low_)
    this->y_low_ = y;
  if (x > this->x_high_)
    this->x_high_ = x;
  if (y > this->y_high_)
    this->y_high_ = y;
}

void MipiDsiRpi::fill(Color color) {
  if (!this->check_buffer_())
    return;
  if (this->get_clipping().is_set()) {
    Display::fill(color);
    return;
  }
  switch (this->color_depth_) {
    case display::COLOR_BITNESS_565: {
      auto *ptr_16 = reinterpret_cast<uint16_t *>(this->buffer_);
      uint8_t hi_byte = static_cast<uint8_t>(color.r & 0xF8) | (color.g >> 5);
      uint8_t lo_byte = static_cast<uint8_t>((color.g & 0x1C) << 3) | (color.b >> 3);
      uint16_t new_color = lo_byte | (hi_byte << 8);
      std::fill_n(ptr_16, this->width_ * this->height_, new_color);
      break;
    }
    case display::COLOR_BITNESS_888:
      for (size_t i = 0; i != this->width_ * this->height_; i++) {
        if (this->color_mode_ == display::COLOR_ORDER_BGR) {
          this->buffer_[i * 3 + 0] = color.b;
          this->buffer_[i * 3 + 1] = color.g;
          this->buffer_[i * 3 + 2] = color.r;
        } else {
          this->buffer_[i * 3 + 0] = color.r;
          this->buffer_[i * 3 + 1] = color.g;
          this->buffer_[i * 3 + 2] = color.b;
        }
      }
      break;
    default:
      break;
  }
  this->x_low_ = 0;
  this->y_low_ = 0;
  this->x_high_ = this->width_ - 1;
  this->y_high_ = this->height_ - 1;
}

int MipiDsiRpi::get_width() {
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_90_DEGREES:
    case display::DISPLAY_ROTATION_270_DEGREES:
      return this->get_height_internal();
    default:
      return this->get_width_internal();
  }
}
int MipiDsiRpi::get_height() {
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_90_DEGREES:
    case display::DISPLAY_ROTATION_270_DEGREES:
      return this->get_width_internal();
    default:
      return this->get_height_internal();
  }
}

void MipiDsiRpi::dump_config() {
  ESP_LOGCONFIG(TAG,
                "MIPI-DSI RPi 7\" V1 / Hosyond"
                "\n  Resolution: %ux%u"
                "\n  DSI lanes: %u @ %.0f Mbps"
                "\n  Pixel clock: %.2f MHz"
                "\n  HSync (pw/bp/fp): %u/%u/%u"
                "\n  VSync (pw/bp/fp): %u/%u/%u",
                (unsigned) this->width_, (unsigned) this->height_, this->lanes_, this->lane_bit_rate_,
                this->pclk_frequency_, this->hsync_pulse_width_, this->hsync_back_porch_, this->hsync_front_porch_,
                this->vsync_pulse_width_, this->vsync_back_porch_, this->vsync_front_porch_);
  LOG_I2C_DEVICE(this);
}

}  // namespace esphome::mipi_dsi_rpi
#endif  // USE_ESP32_VARIANT_ESP32P4

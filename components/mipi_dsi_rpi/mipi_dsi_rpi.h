//
// ESPHome external component: MIPI-DSI driver for the Raspberry Pi 7" Touch Display V1
// (and "driver-free" 800x480 clones such as the Hosyond), on ESP32-P4.
//
#pragma once

#ifdef USE_ESP32_VARIANT_ESP32P4
#include "esphome/core/component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/gpio.h"

#include "esphome/components/display/display.h"
#include "esphome/components/i2c/i2c.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"

namespace esphome::mipi_dsi_rpi {

constexpr static const char *const TAG = "display.mipi_dsi_rpi";

class MipiDsiRpi : public display::Display, public i2c::I2CDevice {
 public:
  MipiDsiRpi(size_t width, size_t height, display::ColorBitness color_depth, uint8_t pixel_mode)
      : width_(width), height_(height), color_depth_(color_depth), pixel_mode_(pixel_mode) {}

  display::ColorOrder get_color_mode() { return this->color_mode_; }
  void set_color_mode(display::ColorOrder color_mode) { this->color_mode_ = color_mode; }
  void set_invert_colors(bool invert_colors) { this->invert_colors_ = invert_colors; }
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_COLOR; }

  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }

  void set_pclk_frequency(float v) { this->pclk_frequency_ = v; }
  void set_lane_bit_rate(float v) { this->lane_bit_rate_ = v; }
  void set_lanes(uint8_t v) { this->lanes_ = v; }
  void set_hsync_pulse_width(uint16_t v) { this->hsync_pulse_width_ = v; }
  void set_hsync_back_porch(uint16_t v) { this->hsync_back_porch_ = v; }
  void set_hsync_front_porch(uint16_t v) { this->hsync_front_porch_ = v; }
  void set_vsync_pulse_width(uint16_t v) { this->vsync_pulse_width_ = v; }
  void set_vsync_back_porch(uint16_t v) { this->vsync_back_porch_ = v; }
  void set_vsync_front_porch(uint16_t v) { this->vsync_front_porch_ = v; }

  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_backlight(uint8_t brightness);

  void setup() override;
  void update() override;
  void dump_config() override;

  void draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) override;
  void draw_pixel_at(int x, int y, Color color) override;
  void fill(Color color) override;
  int get_width() override;
  int get_height() override;

 protected:
  void write_to_display_(int x_start, int y_start, int w, int h, const uint8_t *ptr, int x_offset, int y_offset,
                         int x_pad);
  bool check_buffer_();
  void smark_failed_(const char *message, esp_err_t err);

  esp_err_t attiny_write_register_(uint8_t reg, uint8_t val);
  esp_err_t attiny_read_register_(uint8_t reg, uint8_t *val);
  esp_err_t lcd_power_on_sequence_();
  esp_err_t release_bridge_reset_and_wake_();
  esp_err_t release_touch_reset_();
  void tc358762_reg_write_(uint16_t reg, uint32_t val);
  esp_err_t tc358762_bridge_init_();

  size_t width_{};
  size_t height_{};

  uint16_t hsync_pulse_width_{2};
  uint16_t hsync_back_porch_{46};
  uint16_t hsync_front_porch_{210};
  uint16_t vsync_pulse_width_{20};
  uint16_t vsync_back_porch_{4};
  uint16_t vsync_front_porch_{22};
  float pclk_frequency_{25.98f};
  float lane_bit_rate_{600};
  uint8_t lanes_{1};

  bool invert_colors_{};
  display::ColorOrder color_mode_{display::COLOR_ORDER_RGB};
  display::ColorBitness color_depth_;
  uint8_t pixel_mode_{};
  bool lcd_enabled_{false};

  uint8_t attiny_fw_id_{0xAB};
  bool attiny_id_read_ok_{false};
  uint16_t attiny_write_fails_{0};
  const char *setup_stage_{"not started"};

  esp_lcd_panel_handle_t handle_{};
  esp_lcd_dsi_bus_handle_t bus_handle_{};
  SemaphoreHandle_t io_lock_{};
  uint8_t *buffer_{nullptr};
  uint16_t x_low_{1};
  uint16_t y_low_{1};
  uint16_t x_high_{0};
  uint16_t y_high_{0};
};

}  // namespace esphome::mipi_dsi_rpi
#endif  // USE_ESP32_VARIANT_ESP32P4

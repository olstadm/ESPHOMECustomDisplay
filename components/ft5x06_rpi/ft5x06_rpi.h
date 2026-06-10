#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/components/touchscreen/touchscreen.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/hal.h"

namespace esphome::ft5x06_rpi {

// Raspberry Pi 7" panel FT5406 reports a non-standard vendor ID (commonly 0x00).
// This driver is ESPHome's stock ft5x06 minus the vendor-ID validation gate.

enum FTCmd : uint8_t {
  FT5X06_MODE_REG = 0x00,
  FT5X06_VENDOR_ID_REG = 0xA8,
  FT5X06_TD_STATUS = 0x02,
  FT5X06_TOUCH_DATA = 0x03,
};

enum FTMode : uint8_t {
  FT5X06_OP_MODE = 0,
};

static const size_t MAX_TOUCHES = 5;

class FT5x06RpiTouchscreen : public touchscreen::Touchscreen, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  void update_touches() override;

  void set_interrupt_pin(InternalGPIOPin *interrupt_pin) { this->interrupt_pin_ = interrupt_pin; }

 protected:
  void continue_setup_();
  bool err_check_(i2c::ErrorCode err, const char *msg);
  bool set_mode_(FTMode mode);
  uint8_t vendor_id_{0};
  InternalGPIOPin *interrupt_pin_{nullptr};
};

}  // namespace esphome::ft5x06_rpi

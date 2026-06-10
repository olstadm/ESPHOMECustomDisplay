import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display, i2c
from esphome.components.esp32 import VARIANT_ESP32P4, only_on_variant
from esphome.const import (
    CONF_COLOR_ORDER,
    CONF_ID,
    CONF_INVERT_COLORS,
    CONF_LAMBDA,
)

from . import mipi_dsi_rpi_ns

# ESP32-P4 only, and needs the LDO (for the DSI PHY) + PSRAM (framebuffer).
DEPENDENCIES = ["esp32", "esp_ldo", "psram", "i2c"]

MipiDsiRpi = mipi_dsi_rpi_ns.class_("MipiDsiRpi", display.Display, i2c.I2CDevice)

ColorOrder = display.display_ns.enum("ColorMode")
ColorBitness = display.display_ns.enum("ColorBitness")
COLOR_ORDERS = {
    "RGB": ColorOrder.COLOR_ORDER_RGB,
    "BGR": ColorOrder.COLOR_ORDER_BGR,
}

CONF_LANES = "lanes"
CONF_LANE_BIT_RATE = "lane_bit_rate"  # Mbps
CONF_PCLK_FREQUENCY = "pclk_frequency"
CONF_HSYNC_PULSE_WIDTH = "hsync_pulse_width"
CONF_HSYNC_BACK_PORCH = "hsync_back_porch"
CONF_HSYNC_FRONT_PORCH = "hsync_front_porch"
CONF_VSYNC_PULSE_WIDTH = "vsync_pulse_width"
CONF_VSYNC_BACK_PORCH = "vsync_back_porch"
CONF_VSYNC_FRONT_PORCH = "vsync_front_porch"

# Fixed panel geometry (RPi 7" V1 / Hosyond 800x480, RGB888).
WIDTH = 800
HEIGHT = 480

CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(MipiDsiRpi),
            cv.Optional(CONF_COLOR_ORDER, default="RGB"): cv.enum(
                COLOR_ORDERS, upper=True
            ),
            cv.Optional(CONF_INVERT_COLORS, default=False): cv.boolean,
            cv.Optional(CONF_LANES, default=1): cv.int_range(min=1, max=2),
            cv.Optional(CONF_LANE_BIT_RATE, default=600): cv.int_range(
                min=100, max=1500
            ),
            cv.Optional(CONF_PCLK_FREQUENCY, default="25.98MHz"): cv.All(
                cv.frequency, cv.Range(min=4e6, max=100e6)
            ),
            # Timings default to the embenix RPi 7" V1 profile; override only if needed.
            cv.Optional(CONF_HSYNC_PULSE_WIDTH, default=2): cv.int_,
            cv.Optional(CONF_HSYNC_BACK_PORCH, default=46): cv.int_,
            cv.Optional(CONF_HSYNC_FRONT_PORCH, default=210): cv.int_,
            cv.Optional(CONF_VSYNC_PULSE_WIDTH, default=20): cv.int_,
            cv.Optional(CONF_VSYNC_BACK_PORCH, default=4): cv.int_,
            cv.Optional(CONF_VSYNC_FRONT_PORCH, default=22): cv.int_,
        }
    ).extend(i2c.i2c_device_schema(0x45)),  # ATTINY88 control MCU
    cv.only_on_esp32,
    only_on_variant(supported=[VARIANT_ESP32P4]),
)


async def to_code(config):
    # Buffer/pixel format is fixed RGB888 (888 bitness, 24-bit pixel mode).
    var = cg.new_Pvariable(
        config[CONF_ID], WIDTH, HEIGHT, ColorBitness.COLOR_BITNESS_888, 24
    )
    await display.register_display(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_color_mode(config[CONF_COLOR_ORDER]))
    cg.add(var.set_invert_colors(config[CONF_INVERT_COLORS]))
    cg.add(var.set_lanes(config[CONF_LANES]))
    cg.add(var.set_lane_bit_rate(config[CONF_LANE_BIT_RATE]))
    cg.add(var.set_pclk_frequency(config[CONF_PCLK_FREQUENCY] / 1.0e6))
    cg.add(var.set_hsync_pulse_width(config[CONF_HSYNC_PULSE_WIDTH]))
    cg.add(var.set_hsync_back_porch(config[CONF_HSYNC_BACK_PORCH]))
    cg.add(var.set_hsync_front_porch(config[CONF_HSYNC_FRONT_PORCH]))
    cg.add(var.set_vsync_pulse_width(config[CONF_VSYNC_PULSE_WIDTH]))
    cg.add(var.set_vsync_back_porch(config[CONF_VSYNC_BACK_PORCH]))
    cg.add(var.set_vsync_front_porch(config[CONF_VSYNC_FRONT_PORCH]))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))

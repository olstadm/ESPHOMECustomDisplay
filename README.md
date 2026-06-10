# ESPHome Custom Display — RPi 7" V1 / Hosyond MIPI-DSI (ESP32-P4)

An external ESPHome `display` component that drives the **Raspberry Pi 7" Touch Display V1**
(and 800×480 "driver-free" clones such as the **Hosyond**) on the **ESP32-P4** over MIPI-DSI.

It handles what stock ESPHome can't express for this panel:
- the **ATTINY88** (I²C 0x45) power-on / reset / backlight choreography,
- the **TC358762** DSI→DPI bridge configuration via DSI Generic Long Writes,
- the low-level DSI host overrides (non-burst sync-pulses, continuous HS clock, frame/cmd
  ACK disable) the panel requires.

Rendering/LVGL plumbing is kept verbatim from ESPHome's stock `mipi_dsi` so existing
`lvgl:` configs bind unchanged.

## Usage

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/olstadm/ESPHOMECustomDisplay
      ref: main
    components: [mipi_dsi_rpi]

display:
  - platform: mipi_dsi_rpi
    id: my_display
    i2c_id: bus_a          # bus carrying ATTINY88 (0x45) + FT5x06 touch (0x38)
    # rotation: 180        # uncomment if the image comes up flipped
```

Requires `esp32` (P4 variant), `esp_ldo` (DSI PHY power), `psram`, and an `i2c` bus.

## Credits
- Panel bring-up ported from [embenix/ESP32-P4-DSI-Support-Hub](https://github.com/embenix/ESP32-P4-DSI-Support-Hub)
- Rendering/LVGL integration adapted from ESPHome's stock `mipi_dsi` component

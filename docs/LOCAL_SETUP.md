# Local development notes

The public, cross-platform setup procedure lives in the repository [README](../README.md). This page records details useful to maintainers.

## Service ports

- Dashboard development server: `http://localhost:5173`
- USB bridge HTTP API: `http://localhost:8787`
- USB bridge WebSocket: `ws://localhost:8787/ws`

## Verified hardware

- Board profile: `esp32:esp32:esp32` (ESP32 Dev Module)
- Detected chip: ESP32-D0WD-V3
- USB interface: Silicon Labs CP210x
- Serial speed: 115200 baud
- OLED library: Adafruit SSD1306
- Graphics library: Adafruit GFX

## Validation

Run the portable application checks from the repository root:

```bash
npm run setup
npm test
```

Compile the firmware separately because it requires Arduino CLI and the ESP32 platform:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/water_level_sensor
```

Hardware upload and live serial tests remain manual because CI does not have an attached ESP32.

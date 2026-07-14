# Contributing

Thank you for improving the ESP32 Water Level Monitor.

## Development setup

1. Fork and clone the repository.
2. Install Node.js 20 or newer.
3. Run `npm run setup` from the repository root.
4. Run `npm test` before submitting changes.

For firmware changes, install the ESP32 Arduino core and Adafruit display libraries described in the main README, then compile with:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/water_level_sensor
```

## Pull requests

- Keep changes focused and explain the user-visible behavior.
- Update `SPEC.md` first if a change alters the product contract or API shape.
- Include manual test steps for hardware-dependent changes.
- Do not commit `node_modules`, build output, local configuration, credentials, or device-specific files.

# Firmware Notes

This folder contains the ESP32 sketch for the water level sensor project.

## Current State

- The initial sketch emits newline-delimited JSON over serial at `115200`.
- The sketch keeps OLED feedback enabled for local hardware visibility.
- Wi-Fi endpoints are not implemented yet. The current milestone is the USB bridge path.

## Serial Contract

Each line is a JSON object like:

```json
{"distanceCm":42.3,"rawDurationUs":2488,"readingState":"ok"}
```

## Next Firmware Work

1. Add Wi-Fi provisioning strategy.
2. Expose `/api/status`, `/api/config`, and `/api/reading`.
3. Push live readings over `/ws`.

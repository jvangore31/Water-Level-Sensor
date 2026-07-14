# Firmware Notes

This folder contains the ESP32 sketch for the water level sensor project.

## Current State

- The sketch emits newline-delimited JSON over serial at `115200` for USB bridge compatibility.
- It joins a saved 2.4 GHz Wi-Fi network in station mode and hosts the dashboard, REST API, and WebSocket endpoint.
- A first-run `WaterLevel-XXXX` setup access point provides captive provisioning and recovery.
- The setup access point has no internet route. It closes within 15 seconds after the ESP32 joins the selected normal network, allowing the client to return to its usual Wi-Fi.
- OLED feedback remains enabled for local readings and network addresses.

## Serial Contract

Each line is a JSON object like:

```json
{"distanceCm":42.3,"rawDurationUs":2488,"readingState":"ok"}
```

## Wi-Fi setup

1. On first boot, join `WaterLevel-XXXX` from a phone or computer. A temporary **No internet** warning is expected.
2. Open `http://192.168.4.1` if the setup page does not appear automatically.
3. Select the same 2.4 GHz network used by the phone or computer and enter its password.
4. After success, reconnect the phone or computer to that normal network.
5. Open the displayed IP address or `http://water-level.local`.

Normal monitoring never requires the client to remain connected to `WaterLevel-XXXX`.

The same firmware build is used for additional sensors. Each unit receives its own network credentials and hostname through provisioning; see [`../docs/SENSOR_DEPLOYMENT.md`](../docs/SENSOR_DEPLOYMENT.md).

## Build footprint

With ESP32 Arduino core 3.3.10 and the default `esp32:esp32:esp32` partition, the current embedded-dashboard build uses 1,245,028 of 1,310,720 application bytes (94%) and 53,024 of 327,680 global-variable bytes (16%). Recheck these values after every dashboard or firmware dependency update.

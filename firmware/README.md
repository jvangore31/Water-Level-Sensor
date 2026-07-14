# Firmware Notes

This folder contains the ESP32 sketch for the water level sensor project.

## Current State

- The sketch emits newline-delimited JSON over serial at `115200` for USB bridge compatibility.
- It joins a saved 2.4 GHz Wi-Fi network in station mode and hosts the dashboard, REST API, and WebSocket endpoint.
- An open `WaterLevel-XXXX` access point provides captive provisioning on first run and the complete dashboard during router outages.
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

No sensor dashboard or maintenance-AP password is required. If the saved router is unavailable, the open maintenance AP starts after 30 seconds by default; join it and open `http://192.168.4.1` for the full dashboard. The router's own Wi-Fi password is still stored and used when joining that router.

The same firmware build is used for additional sensors. Each unit receives its own network credentials and hostname through provisioning; see [`../docs/SENSOR_DEPLOYMENT.md`](../docs/SENSOR_DEPLOYMENT.md).

## Build footprint

This release requires the **Minimal SPIFFS (1.9 MB APP with OTA/128 KB SPIFFS)** partition to preserve application headroom. Build with:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 --board-options PartitionScheme=min_spiffs firmware/water_level_sensor
```

Select the same partition scheme for upload. Recheck flash and RAM use after every dashboard or firmware dependency update.

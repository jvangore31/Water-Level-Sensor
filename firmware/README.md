# Firmware Notes

This folder contains the ESP32 sketch for the water level sensor project.

## Current State

- The sketch emits newline-delimited JSON over serial at `115200` for USB bridge compatibility.
- It joins a saved 2.4 GHz Wi-Fi network in station mode and hosts the dashboard, REST API, and WebSocket endpoint.
- A protected `WaterLevel-XXXX` access point provides captive provisioning on first run and the complete dashboard during router outages.
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

After Wi-Fi setup, open the dashboard and sign in once with the generic deployment bootstrap credential. The dashboard requires creation of a new administrator password and a different maintenance-AP password before monitoring. The bootstrap credential is then disabled for that device.

The generic credential is configured at build time with `WATER_LEVEL_BOOTSTRAP_CREDENTIAL`; the development fallback is `waterlevel-setup`. Change it for deployment builds. The same firmware build can be used for additional sensors, but uncommissioned devices sharing a bootstrap credential must be prepared in a controlled setting.

If the saved router is unavailable, the protected maintenance AP starts after 30 seconds by default. Join it with the device's maintenance-AP password and open `http://192.168.4.1` for the full dashboard.

The same firmware build is used for additional sensors. Each unit receives its own network credentials and hostname through provisioning; see [`../docs/SENSOR_DEPLOYMENT.md`](../docs/SENSOR_DEPLOYMENT.md).

## Build footprint

This release requires the **Minimal SPIFFS (1.9 MB APP with OTA/128 KB SPIFFS)** partition to preserve application headroom. Build with:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 --board-options PartitionScheme=min_spiffs firmware/water_level_sensor
```

Select the same partition scheme for upload. Recheck flash and RAM use after every dashboard or firmware dependency update.

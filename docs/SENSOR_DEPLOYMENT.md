# Sensor Deployment Guide

This project supports two deployment options from the same ESP32 firmware:

1. **Standalone Wi-Fi:** the ESP32 hosts the dashboard, REST API, and WebSocket server. A PC and the Node.js bridge are not required after setup.
2. **USB bridge:** the ESP32 sends the same readings over USB serial to the optional Node.js bridge and development dashboard.

Wi-Fi and USB serial can operate at the same time.

## Preparing another sensor

Flash the same sketch onto every supported ESP32:

```bash
npm run build:firmware-assets
arduino-cli compile --fqbn esp32:esp32:esp32 --board-options PartitionScheme=min_spiffs firmware/water_level_sensor
arduino-cli upload --fqbn esp32:esp32:esp32 --board-options PartitionScheme=min_spiffs --port YOUR_PORT firmware/water_level_sensor
```

Do not add a Wi-Fi name or password to the source code. Each ESP32 stores its own credentials and device name in non-volatile storage after provisioning.

For a new or Wi-Fi-reset sensor:

1. Power the sensor and join its unique `WaterLevel-XXXX` setup network.
2. Join with the deployment bootstrap credential and open `http://192.168.4.1` if the captive setup page does not open automatically.
3. Select the same 2.4 GHz local network used by the viewing phone or computer.
4. Choose a unique hostname when deploying multiple sensors, such as `water-level-garage` or `water-level-cistern`.
5. After setup succeeds, reconnect the phone or computer to the normal network.
6. Open the hostname shown by setup, for example `http://water-level-garage.local`, or use the displayed IPv4 address.
7. Sign in with the bootstrap credential, then create a unique administrator password and a different maintenance-AP password. Record both in the controlled deployment handoff.

The temporary setup network has no internet route and closes within 15 seconds of a successful connection. Normal monitoring occurs through the existing router, not through `WaterLevel-XXXX`.

## Moving a sensor away from its configured network

The sensor is a local-network device; it does not use a cloud relay or cellular connection.

When its saved Wi-Fi is unavailable, the firmware:

1. Continues measuring water level, updating the OLED, and emitting USB serial readings.
2. Marks network access as disconnected and retries the saved Wi-Fi with increasing delays, capped at 60 seconds.
3. Keeps the saved credentials; a temporary outage does not erase them.
4. Starts the protected `WaterLevel-XXXX` maintenance AP after 30 seconds by default while continuing to retry the saved network.
5. Allows an administrator to view the complete dashboard or provision a different 2.4 GHz network at `http://192.168.4.1`.
6. Saves the replacement credentials only after the new connection succeeds, then closes the setup network.

If the sensor comes back within range of its saved Wi-Fi, it reconnects automatically. Power-cycling starts the 10-minute recovery timer again.

While the sensor is away from a reachable Wi-Fi network, its browser dashboard cannot be reached, but sensing, OLED display, and USB output continue. Offline readings are not persisted or uploaded later; after reconnection, clients receive the current reading and new live readings. The sensor cannot be accessed remotely over the public internet unless a separate, secured remote-access design is implemented in the future.

## Changing networks deliberately

Use one of these methods:

- While the current dashboard is reachable, choose **Change Wi-Fi network** and confirm the reset.
- Over trusted physical USB serial, send `WIFI_RESET WaterLevel-XXXX` at 115200 baud, using the exact AP name shown by the device.
- At a new location, wait for the configured AP delay, join the protected `WaterLevel-XXXX` network, and sign in. This method retains the previous credentials until the replacement network connects successfully.

The first two reset methods erase only the Wi-Fi SSID and password. Container depth, tank name, and alert thresholds are retained.

## Network limitations

- ESP32 V1 hardware supports 2.4 GHz Wi-Fi, not 5 GHz-only networks.
- Both devices must be on a network that permits communication between clients.
- Guest, apartment, hotel, and managed networks may enable client isolation or require browser sign-in. Those networks can prevent local dashboard access even when both devices show the same Wi-Fi name.
- `.local` names depend on mDNS support. The displayed IPv4 address is the fallback.
- An IP address assigned by DHCP can change after a router restart. Prefer the configured `.local` hostname where supported.

## Factory preparation and credentials

A normal firmware upload does not necessarily erase the ESP32's non-volatile preferences. Before giving a previously configured unit to another user or location, reset its Wi-Fi credentials from the dashboard or use the documented device-specific USB reset command. Verify that its unique `WaterLevel-XXXX` network appears before deployment.

Wi-Fi passwords are write-only: they are never returned by the device API, dashboard, WebSocket messages, or normal serial logs.

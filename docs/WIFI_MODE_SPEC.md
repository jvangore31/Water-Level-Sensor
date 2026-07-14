# Wi-Fi Standalone Mode Specification

Status: In implementation

Target milestone: Milestone 3

Parent specification: [`SPEC.md`](../SPEC.md)

## 1. Purpose

Wi-Fi standalone mode allows the water-level sensor to operate without a USB connection to a computer. The ESP32 is powered from a wall adapter or other suitable supply, connects to a local Wi-Fi network, hosts the dashboard and device API, and sends live readings directly to phones, tablets, and computers on that network.

USB remains supported for initial firmware installation, diagnostics, recovery, and environments where Wi-Fi is unavailable.

### 1.1 Network topology and internet behavior

The sensor's normal operating network is the user's existing local Wi-Fi. The PC, phone, and sensor are peers on that network; the PC does not connect directly to the sensor and the sensor does not replace the router.

`WaterLevel-XXXX` is a temporary, setup-only access point. It intentionally has no internet gateway and must never be presented as the network used for normal monitoring. A phone or PC connected to it may report **No internet** during provisioning. After credentials are accepted, the sensor joins the selected local network, closes the setup access point within 15 seconds, and directs the user to reconnect to that same local network. Normal internet access continues through the user's router while sensor readings travel locally between the browser and ESP32.

The firmware is not required to route, bridge, or provide NAT between the setup access point and the internet.

## 2. User outcome

After initial setup, a user must be able to:

1. Power the ESP32 without connecting it to a computer.
2. Open `http://water-level.local` from a device on the same local network.
3. View live water level, depth, distance, health, and last-update time.
4. Change container and alert configuration.
5. Restart the ESP32 without repeating Wi-Fi setup.
6. Recover or replace Wi-Fi credentials without reflashing firmware.

The Node.js USB bridge and a continuously running PC must not be required in Wi-Fi mode.

## 3. System architecture

```text
                         Local Wi-Fi network

  Phone / tablet / PC  <---------------------->  ESP32 water-level sensor
  Browser dashboard          HTTP + WS              ├─ sensor sampling
                                                     ├─ REST API
                                                     ├─ WebSocket server
                                                     ├─ dashboard files
                                                     ├─ Wi-Fi credentials
                                                     └─ device configuration
```

The ESP32 must provide:

- station-mode Wi-Fi connectivity,
- first-run provisioning access point,
- HTTP server,
- WebSocket server,
- mDNS hostname,
- static dashboard assets stored in flash,
- persistent Wi-Fi and container configuration,
- serial output compatible with the existing USB bridge.

## 4. Operating modes

### 4.1 Wi-Fi station mode

This is the normal standalone operating mode.

- The ESP32 connects to the configured 2.4 GHz Wi-Fi network.
- The ESP32 advertises `water-level.local` using mDNS.
- The dashboard, REST API, and WebSocket are served directly by the ESP32.
- Serial JSON output continues at 115200 baud when USB is attached.
- Sensor sampling must continue if no browser is connected.
- Station-only mode is used after a saved network connects; the setup SSID must not remain available during normal operation.

### 4.2 Provisioning access-point mode

Provisioning mode is used when:

- no Wi-Fi credentials have been saved,
- the saved network cannot be reached after the defined retry period, or
- the user intentionally requests a network reset.

In this mode the ESP32 creates a temporary network:

```text
SSID: WaterLevel-XXXX
Address: http://192.168.4.1
```

`XXXX` is derived from the final four hexadecimal characters of the device MAC address so multiple unconfigured sensors can be distinguished.

This access point is only a local path to the setup page. It has no internet access. Losing internet connectivity while explicitly joined to it is expected and limited to the provisioning session.

The setup page must allow the user to:

- scan for nearby 2.4 GHz networks,
- enter an SSID manually,
- enter the network password,
- set the device name/hostname,
- save and attempt the connection,
- see a clear success or failure result.

The Wi-Fi password must never be returned by an API, displayed after saving, written to normal serial logs, or included in diagnostic exports.

### 4.3 USB bridge mode

USB mode remains compatible with the existing bridge. Wi-Fi features must not change the newline-delimited serial contract.

USB and Wi-Fi may operate simultaneously. Both transports expose the latest reading, but configuration ownership follows Section 11.

## 5. First-run flow

1. User uploads the firmware once by USB.
2. User powers the ESP32 from a computer or wall adapter.
3. With no saved credentials, the ESP32 starts `WaterLevel-XXXX`.
4. User connects a phone or computer to that network.
5. A dashboard-style captive setup page opens automatically where supported; `http://192.168.4.1` is the documented fallback.
6. User selects a scanned 2.4 GHz network, enters its password, and chooses **Connect**.
7. The setup page validates the input and shows connection progress without requiring a manual page refresh.
8. ESP32 saves the credentials only after it successfully joins the selected network.
9. On success, the setup page shows the chosen hostname, assigned IP address, setup-network shutdown countdown, and a **Go to dashboard** action.
10. Within 15 seconds of success, the ESP32 disables `WaterLevel-XXXX`. The user reconnects to the selected normal Wi-Fi network if the operating system does not do so automatically.
11. User opens `http://water-level.local`, the configured hostname, or the displayed IP address.
12. The full dashboard loads from the ESP32 and begins receiving live readings.

### 5.1 Provisioning user interface

The provisioning page is a focused setup version of the dashboard, visually consistent with the main application. It must be usable from a phone without installing an application.

The setup experience consists of three screens:

#### Welcome

- Identify the product as **Water Level Sensor**.
- Explain that setup connects the sensor to the user's local 2.4 GHz Wi-Fi.
- Show the device identifier matching the provisioning SSID suffix.
- Provide one primary **Set up Wi-Fi** action.

#### Choose network

- Display scanned networks ordered by signal strength.
- Show signal strength and whether each network is secured.
- Clearly label that only 2.4 GHz networks are supported.
- Provide **Scan again** and **Enter network manually** actions.
- Request the Wi-Fi password only after a network is selected.
- Include a show/hide password control.
- Allow an optional device name, defaulting to `water-level`.
- Disable **Connect** until required fields are valid.
- Never display or log the saved password after submission.

#### Connection result

- Show distinct progress states: saving, connecting, obtaining an address, and ready.
- Keep the setup access point available while showing a failed connection result.
- On failure, explain whether the network was not found, authentication failed, or the connection timed out when the platform can distinguish those cases.
- Allow the user to correct the password or choose another network without resetting or reflashing the device.
- On success, show `http://<hostname>.local` and the assigned IPv4 address.
- Explain before setup and again on success that `WaterLevel-XXXX` has no internet access and is temporary.
- Explain that the phone or computer must reconnect to the selected normal Wi-Fi before opening the dashboard.
- Show how soon the temporary setup access point will close; it must close within 15 seconds after the success response is available to the browser.
- Provide a **Go to dashboard** button. If direct navigation fails because the client is still attached to the setup network, keep the address visible and provide clear reconnection instructions.

### 5.2 Captive portal behavior

- While provisioning is active, common operating-system connectivity-check requests should redirect to the setup page.
- DNS queries received on the setup network should resolve to `192.168.4.1` where supported by the captive portal implementation.
- The complete setup page must work without internet access or externally hosted fonts, scripts, styles, or images.
- The setup page must remain reachable at `http://192.168.4.1` even when automatic captive-portal detection does not work.
- The ESP32 must not require the user to type its IP address when the operating system successfully opens the captive portal.
- Provisioning HTTP requests must use bounded sizes and timeouts so malformed clients cannot stop sensor sampling.

### 5.3 Setup from the USB dashboard

When the ESP32 is connected by USB, the normal dashboard should offer an additional **Set up sensor Wi-Fi** action. This provides a convenient alternative but is not required for phone-only first-run setup.

The USB setup flow uses a bidirectional serial command protocol to:

- request a Wi-Fi scan,
- submit an SSID, password, and hostname,
- receive connection progress,
- report the final hostname and IP address,
- reset saved network credentials after confirmation.

Wi-Fi passwords submitted through USB must be treated as write-only values. The ESP32 and bridge must never return the stored password. The serial provisioning command protocol must be specified separately before implementation so it cannot be confused with sensor-reading JSON lines.

The captive setup page remains the primary and required provisioning path because it works when the device is powered from a wall adapter and does not require the Node.js bridge.

## 6. Network requirements

- V1 supports 2.4 GHz Wi-Fi using WPA2-Personal or open networks supported by the ESP32 platform.
- 5 GHz-only networks are not supported by the target ESP32 hardware.
- Enterprise Wi-Fi, captive hotel networks, and networks requiring browser sign-in are out of scope for V1.
- The device obtains an address through DHCP.
- Static IP configuration is out of scope for V1.
- The default hostname is `water-level`.
- A custom hostname must contain only lowercase letters, numbers, and hyphens, with a maximum length of 32 characters.
- If mDNS is unavailable on a client or network, the dashboard must display the assigned IPv4 address on the OLED and through serial logs.

## 7. ESP-hosted dashboard

The production dashboard build must be stored in ESP32 flash and served at `/`.

Requirements:

- Direct navigation to `/` returns the dashboard.
- Static assets use compressed representations when feasible.
- Unknown non-API routes fall back to the dashboard entry page.
- API and WebSocket URLs are relative to the page origin in standalone mode.
- The same React dashboard source is used for USB and Wi-Fi modes.
- The dashboard clearly identifies the active transport as `Wi-Fi`.
- The dashboard remains responsive on current phone, tablet, and desktop browsers.
- If the WebSocket disconnects, the dashboard shows a stale/offline state and retries with bounded backoff.

The firmware build process must document how to build the dashboard and upload or embed its generated assets.

## 8. Device API

The ESP32 must implement the shared contract from `SPEC.md`.

### 8.1 REST endpoints

#### `GET /api/status`

Returns device and network state:

```json
{
  "mode": "wifi",
  "deviceConnected": true,
  "sensorHealthy": true,
  "firmwareVersion": "0.2.0",
  "serialPort": null,
  "ipAddress": "192.168.1.40",
  "hostname": "water-level.local",
  "wifiConnected": true,
  "wifiSignalDbm": -58,
  "uptimeSeconds": 86400,
  "status": "reading",
  "message": "Receiving sensor readings."
}
```

The response must not contain the Wi-Fi password.

#### `GET /api/config`

Returns the persisted container and alert configuration.

#### `PUT /api/config`

Validates, persists, and returns the complete configuration. Invalid input returns HTTP 400 with a useful error body.

#### `GET /api/reading`

Returns the latest reading using the shared reading shape.

#### `GET /api/network`

Returns non-secret network information:

```json
{
  "configured": true,
  "connected": true,
  "ssid": "HomeNetwork",
  "hostname": "water-level",
  "ipAddress": "192.168.1.40",
  "signalDbm": -58
}
```

#### `POST /api/network/reset`

Erases saved Wi-Fi credentials after an explicit confirmation value, then restarts into provisioning mode. This endpoint must not erase container calibration unless specifically requested by a separate action.

### 8.2 WebSocket

The WebSocket endpoint remains `/ws` and sends:

- `status` events,
- `reading` events,
- `config` events.

On connection, the server immediately sends one snapshot of all three types. New reading events are sent at the sensor sampling interval. Slow or disconnected clients must not block sensor sampling.

## 9. Sensor behavior

- Sample the ultrasonic sensor every 500 ms by default.
- Continue newline-delimited serial JSON output at 115200 baud.
- Continue sensor operation if Wi-Fi disconnects.
- Continue sensor and network operation if OLED initialization fails; report the display failure as a degraded status instead of halting.
- Represent timeouts and invalid measurements with `distanceCm: null` and the appropriate `readingState`.
- Mark the sensor unhealthy when no new sample has been produced within three sampling intervals.
- Apply a documented median or similar filter before publishing readings to reduce single-sample spikes.

## 10. Persistent storage

The ESP32 must store the following in non-volatile storage:

- Wi-Fi SSID and password,
- device hostname,
- container depth,
- container display name,
- warning threshold,
- critical threshold,
- calibration values introduced later,
- a configuration schema version.

Requirements:

- Settings survive power loss and firmware restart.
- Writes occur only when values change, not on every sensor sample.
- Invalid or incompatible saved data falls back to safe defaults.
- Firmware upgrades provide a migration path when the schema changes.
- Wi-Fi credentials are never exposed through normal APIs or logs.

## 11. Configuration ownership

In standalone mode, the ESP32 is the source of truth for configuration.

- Wi-Fi dashboard changes are persisted on the ESP32.
- USB bridge configuration changes should be forwarded to the ESP32 when a command protocol is added.
- Until bidirectional serial configuration exists, the bridge must clearly indicate that its local configuration is separate from the ESP32’s Wi-Fi configuration.
- Reading calculations returned by the ESP32 use the ESP32-persisted container depth.

## 12. Connection and recovery behavior

### 12.1 Startup

1. Initialize serial and sensor services.
2. Load and validate persistent configuration.
3. Begin sensor sampling without waiting for Wi-Fi.
4. Attempt the saved Wi-Fi connection.
5. Start normal web services after receiving an IP address.

### 12.2 Lost Wi-Fi

- Keep sampling and displaying readings locally.
- Attempt reconnection using increasing delays capped at 60 seconds.
- Do not erase credentials because of a temporary outage.
- After 10 minutes without a connection, also enable the provisioning access point while continuing station-mode retries.
- Disable the provisioning access point after station mode is stable, unless the user explicitly keeps it enabled during setup.
- When the station connection succeeds, allow at most 15 seconds for the setup browser to receive the result, then disable the provisioning access point and return to station-only mode.
- If station connectivity fails again during that grace period, cancel access-point shutdown so recovery remains available.
- Sensing, OLED updates, and USB serial output continue while the device is outside the saved network's range.
- Offline readings are not persisted for later synchronization in V1; after reconnection, the API exposes the latest sample and resumes live delivery.
- The browser dashboard is unavailable while neither station Wi-Fi nor the setup access point is reachable; V1 does not provide cloud or public-internet access.
- Returning within range of the saved network causes automatic reconnection without reprovisioning.
- Power cycling restarts the 10-minute offline interval before the recovery access point is enabled.

### 12.3 Moving to a different location

The same firmware image is used at every location; Wi-Fi credentials must not be compiled into a per-location image.

- If the old dashboard is still reachable, the user may reset Wi-Fi before moving and provision at the destination.
- Otherwise, after 10 continuous minutes without the saved network, the recovery access point allows provisioning at the destination.
- A replacement SSID, password, and hostname are committed only after a successful station connection. A failed attempt must leave the last working credentials available for retry.
- Moving between locations does not erase container calibration or alert configuration.
- Access from outside the local Wi-Fi network is out of scope. Remote monitoring requires a separately specified authenticated and encrypted service.

### 12.4 Credential reset

At least one recovery mechanism must work without network access:

- holding a documented hardware button during startup, or
- a documented serial command entered over USB.

The selected mechanism must require deliberate action and must be documented in the main README.

### 12.5 Watchdog

The firmware should use a watchdog or equivalent recovery mechanism so a web request, failed network operation, or peripheral problem cannot permanently stop sensor sampling.

## 13. OLED behavior

The OLED rotates through or otherwise displays:

- current distance or water percentage,
- Wi-Fi connection state,
- hostname when connected,
- IPv4 address when connected,
- provisioning SSID and `192.168.4.1` during setup,
- a clear sensor error when readings are invalid.

The display is helpful but not required for API, serial, or sensor operation.

## 14. Security and privacy

V1 is intended for a trusted private local network.

- The firmware does not expose services to the public internet.
- Router port forwarding must not be required or recommended.
- Cloud accounts and cloud relays are out of scope.
- Wi-Fi passwords must not appear in API responses, WebSocket messages, logs, or crash reports.
- State-changing endpoints accept only JSON and validate all fields.
- The setup access point must shut down after successful provisioning.
- If an access-point password is implemented, it must be unique or user-configurable rather than a universal project password.
- HTTPS on the constrained local device is not required for V1; the documentation must state that local HTTP is unencrypted.
- Authentication for the normal local dashboard is deferred. It is required before any future internet-access feature.

## 15. Firmware update strategy

V1 requires USB firmware updates.

Browser-based or Arduino OTA updates are a future enhancement. OTA must not be enabled without authentication and integrity checks.

## 16. Error states

The device and dashboard must distinguish:

- not provisioned,
- connecting to Wi-Fi,
- wrong credentials or network unavailable,
- Wi-Fi connected but mDNS unavailable,
- no sensor data,
- invalid sensor data,
- sensor out of range,
- stale sensor data,
- display unavailable,
- persistent-storage failure,
- low-memory or web-service failure where detectable.

Network failure must not be presented as sensor failure, and sensor failure must not be presented as network failure.

## 17. Performance targets

- First valid sensor reading: within 3 seconds of boot.
- Normal connection to a saved available network: within 15 seconds of boot.
- Dashboard first load on the local network: within 3 seconds under normal conditions.
- Reading delivery latency: under 1 second from sample to dashboard.
- WebSocket reconnection after a brief network interruption: within 10 seconds under normal conditions.
- At least four simultaneous dashboard clients without interrupting sampling.
- Continuous operation target: seven days without manual restart or unrecovered loss of readings.

## 18. Compatibility targets

- ESP32 Dev Module matching the currently tested ESP32-D0WD-V3 class hardware.
- Current Chrome, Edge, Firefox, and Safari desktop browsers.
- Current Chrome on Android and Safari on iOS/iPadOS.
- Common home routers using 2.4 GHz WPA2-Personal.
- Windows, macOS, Linux, Android, and iOS clients on the same local network.

Some guest Wi-Fi networks block communication between clients. This limitation must be documented because it prevents browsers from reaching the ESP32 even when both are connected to the same SSID.

## 19. Acceptance criteria

Wi-Fi standalone mode is complete only when all of the following pass:

1. A factory-reset device exposes the documented provisioning network.
2. A phone can provision the device without installing an application.
3. Credentials survive power removal and restart.
4. The ESP32 runs from a wall USB adapter with no computer attached.
5. The dashboard loads from the ESP32 at `water-level.local` on a compatible client.
6. The assigned IP address works when mDNS is unavailable.
7. Dashboard readings update live over WebSocket.
8. REST snapshots and WebSocket readings use the same data shapes.
9. Container configuration persists after restart.
10. Wi-Fi loss does not stop sensor sampling or serial output.
11. The device reconnects after the router returns.
12. Invalid and out-of-range readings never display a fabricated percentage.
13. Wi-Fi credentials do not appear in APIs, WebSocket traffic, or normal serial logs.
14. A user can reset network credentials without reflashing firmware.
15. USB bridge mode continues to work with the same serial contract.
16. The firmware and embedded dashboard fit within the selected flash partition with documented free space.
17. A 24-hour hardware test completes without a crash, memory exhaustion, or stale dashboard reading.
18. First-time setup can be completed from a current iOS or Android phone without a computer or installed application.
19. The captive setup page works without an internet connection and remains directly available at `192.168.4.1`.
20. A wrong Wi-Fi password produces a recoverable error without erasing previously working credentials or requiring a firmware upload.
21. A user can retry setup, rescan networks, or enter a hidden SSID without restarting the ESP32.
22. The success screen displays both the mDNS hostname and assigned IPv4 address.
23. After successful setup, `WaterLevel-XXXX` disappears within 15 seconds; with the client rejoined to the selected local network, the dashboard is reachable and the client's normal router-provided internet access remains available.

## 20. Implementation phases

### Phase 1: Network foundation

- Add persistent configuration schema.
- Connect to a hard-coded development network without committing credentials.
- Add mDNS and non-secret network status.
- Preserve sensor sampling and serial output during network operations.

### Phase 2: Device API

- Implement status, config, reading, and network REST endpoints.
- Implement WebSocket snapshots and live readings.
- Add validation, stale-reading detection, and automated unit tests where feasible.

### Phase 3: Embedded dashboard

- Build the existing React dashboard for relative-origin operation.
- Store compressed production assets in flash.
- Serve the dashboard and verify live updates from phone and desktop browsers.

### Phase 4: Provisioning and recovery

- Add the setup access point and captive setup page.
- Implement the welcome, network selection, progress, failure, and success screens from Section 5.
- Add credential persistence and reset behavior.
- Add OLED setup and recovery information.
- Test failed credentials, router outages, and network changes.
- Add optional USB-dashboard provisioning after the bidirectional serial command protocol is specified.

### Phase 5: Reliability validation

- Add reconnection backoff and watchdog behavior.
- Perform multi-client, 24-hour, and seven-day soak tests.
- Document flash/RAM usage and supported environments.
- Complete every acceptance criterion.

## 21. Out of scope for initial Wi-Fi release

- Public internet access
- Cloud storage or cloud notifications
- User accounts
- HTTPS certificate management on the ESP32
- Enterprise Wi-Fi
- 5 GHz Wi-Fi
- Static IP configuration
- Automatic OTA updates
- Historical analytics beyond the dashboard’s short in-memory recent-reading list

These features require separate specifications before implementation.

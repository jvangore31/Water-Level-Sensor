# Remote Deployment Hardening Specification

## 1. Status and purpose

This document is the approved design baseline for firmware `0.3.0`. The firmware and embedded dashboard implementation is present in the repository. Hardware-only acceptance work, including power-cycle, watchdog-injection, multi-client, scheduled-sleep cycle, and seven-day soak tests, remains pending.

The purpose of this release is to make the water-level sensor safer and more dependable at a location with intermittent or unavailable internet access. It covers:

- optional power-saving configuration,
- a complete dashboard that works directly through the sensor access point,
- stronger measurement calibration and validation,
- firmware recovery and field diagnostics,
- authentication and safer local-network behavior.

Persistent history, LoRa/cellular telemetry, solar hardware, and remote cloud access are separate projects and are not included here.

## 2. Design principles

1. Sensing must not depend on internet access, NTP, a router, a browser, or the OLED.
2. Existing installations must continue operating after an upgrade without reconfiguration.
3. Every power-saving feature must be opt-in. The default remains continuous operation.
4. A nearby authorized user must be able to view readings and recover the device using only a current phone or computer.
5. Invalid, stale, or implausible measurements must never be presented as valid water levels.
6. A peripheral, network request, or client must not be able to stop sampling permanently.
7. Configuration and administrative actions must require authentication after initial setup.

## 3. Release scope

### 3.1 Included

- Backward-compatible extensions to persistent configuration.
- Configurable sampling interval and OLED timeout.
- Optional scheduled low-power operation.
- Optional battery-voltage measurement when supported by installed hardware.
- Immediate authenticated maintenance access point when station Wi-Fi is unavailable.
- The normal monitoring dashboard on both station Wi-Fi and the maintenance access point.
- Full/empty calibration, sensor blind-zone checks, filtering, and plausibility checks.
- Task watchdogs, stale-reading detection, fault counters, reboot diagnostics, and soak tests.
- Per-device credentials, authenticated sessions, request-origin protection, and login throttling.

### 3.2 Excluded

- Internet-facing access or router port forwarding.
- Cloud accounts, cloud alerts, and cloud storage.
- LoRa, LoRaWAN, cellular, or SMS integration.
- Offline measurement history and later synchronization.
- Automatic or unauthenticated OTA firmware updates.
- Arbitrary tank-shape or volume calculations. This release reports a calibrated linear level percentage.
- Automatic temperature compensation unless a supported temperature sensor is added in a later hardware specification.

## 4. Configuration model

The existing config fields remain valid. New fields are optional on input and receive the defaults below when absent. Firmware must migrate older stored configuration without erasing Wi-Fi credentials or calibration.

```json
{
  "containerDepthCm": 120,
  "containerName": "Main Tank",
  "warningThresholdPercent": 35,
  "criticalThresholdPercent": 15,
  "preferredMode": "wifi",
  "measurement": {
    "calibrationMode": "container_depth",
    "fullDistanceCm": null,
    "emptyDistanceCm": null,
    "minimumValidDistanceCm": 20,
    "maximumValidDistanceCm": 450,
    "medianWindowSize": 5,
    "maximumStepCm": 25,
    "stepConfirmationSamples": 3,
    "invalidSamplesBeforeFault": 3
  },
  "power": {
    "powerSavingEnabled": false,
    "sampleIntervalSeconds": 0.5,
    "displayTimeoutSeconds": 0,
    "scheduledSleepEnabled": false,
    "awakeWindowSeconds": 30,
    "batteryMonitoringEnabled": false,
    "batteryLowVoltage": 3.4,
    "batteryCriticalVoltage": 3.2,
    "batteryCalibrationMultiplier": 1.0
  },
  "network": {
    "maintenanceApEnabled": true,
    "maintenanceApDelaySeconds": 30,
    "maintenanceApIdleTimeoutSeconds": 900
  }
}
```

### 4.1 Compatibility rules

- `calibrationMode: "container_depth"` preserves the existing calculation using `containerDepthCm`.
- `calibrationMode: "full_empty"` uses `fullDistanceCm` and `emptyDistanceCm`.
- Existing clients may omit all new objects.
- `GET /api/config` returns the complete normalized configuration.
- `PUT /api/config` accepts either the complete normalized object or a documented complete object from an older client. It must not silently reset omitted new fields to unsafe values.
- Unknown fields are rejected with a field-specific validation error so configuration mistakes are visible.
- Persistent writes occur only after a validated value changes.

## 5. Optional power-saving behavior

### 5.1 Default continuous profile

When `powerSavingEnabled` is `false`:

- sampling continues at the existing 0.5-second interval unless `sampleIntervalSeconds` is explicitly changed,
- Wi-Fi and the web server remain available,
- scheduled sleep is disabled,
- an OLED timeout may still be configured independently.

This is the post-upgrade behavior for existing devices.

### 5.2 Configurable sampling

- `sampleIntervalSeconds` accepts 0.5 through 3600 seconds.
- The dashboard explains that short intervals increase power consumption and sensor activity.
- Changing the interval takes effect without restarting and does not trigger an immediate persistent write on every sample.
- Freshness and stale-reading limits are derived from the configured interval, not from a hard-coded 0.5-second interval.

### 5.3 OLED timeout

- `displayTimeoutSeconds: 0` means the OLED remains on.
- Values from 10 through 86400 turn the OLED off after that many seconds without local or authenticated dashboard activity.
- Any supported local wake action turns it on. Until a hardware button exists, an authenticated `POST /api/display/wake` request is sufficient.
- Display sleep must not stop sensing, serial output, Wi-Fi recovery, or alerts.
- The display is blanked using the controller's display-off command rather than repeatedly drawing a black frame.

### 5.4 Scheduled sleep

Scheduled sleep is allowed only when both `powerSavingEnabled` and `scheduledSleepEnabled` are true.

- The ESP32 wakes, initializes the sensor, collects enough samples to satisfy the configured median filter, publishes a reading, remains reachable for `awakeWindowSeconds`, and then enters deep sleep.
- `awakeWindowSeconds` accepts 15 through 600 seconds.
- The approved default awake window is 30 seconds.
- The dashboard must warn that the device is intentionally unreachable while asleep.
- Maintenance AP access is available during the awake window when station Wi-Fi is unavailable.
- A device must not enter sleep while configuration is being committed or a firmware upload is in progress.
- If repeated sensor initialization fails, the device records the fault before sleeping and retries on the next wake.
- Because V1 does not persist reading history, scheduled sleep is intended for onsite inspection or future telemetry work; it does not by itself create a historical record.

### 5.5 Battery monitoring

Battery monitoring is optional and must remain disabled on hardware without a protected voltage-divider input.

- Enabling it requires a documented hardware revision with a safe ADC connection.
- Firmware reports measured voltage, configured thresholds, and `normal`, `low`, or `critical` state.
- Threshold validation requires `batteryCriticalVoltage < batteryLowVoltage`.
- Battery calibration changes the reported voltage only; it must not alter unrelated sensor calibration.
- The dashboard must not show a fabricated battery percentage unless a chemistry-specific discharge model is specified later.

## 6. Offline monitoring and maintenance access point

### 6.1 Required behavior

The current provisioning-only fallback is replaced by a combined maintenance and provisioning access point.

- A device with no saved network starts the maintenance AP immediately.
- A device with saved credentials attempts station Wi-Fi without blocking sampling.
- If station Wi-Fi remains unavailable for `maintenanceApDelaySeconds`, the maintenance AP starts while station retries continue.
- The default delay is 30 seconds. Accepted values are 0 through 600 seconds.
- Connecting to the maintenance AP at `http://192.168.4.1` opens the normal dashboard, not only the provisioning wizard.
- The dashboard exposes current readings, health, battery state when enabled, diagnostics, calibration, and network setup.
- Changing Wi-Fi is an authenticated administrative action within the dashboard.
- Successful station reconnection does not interrupt an active authenticated maintenance session. The AP may close after its idle timeout once no clients remain.

### 6.2 AP lifecycle

- `maintenanceApEnabled` defaults to `true` and cannot be disabled until initial administration credentials have been created.
- `maintenanceApIdleTimeoutSeconds: 0` keeps the AP enabled whenever station Wi-Fi is unavailable.
- Values from 60 through 86400 close the AP after that much client inactivity, while station retries continue.
- After timeout, a documented physical action or power cycle makes the AP available again. Until a hardware button is specified, power cycling is the required action.
- Scheduled-sleep devices expose the AP only during their awake window.
- OLED output identifies maintenance AP state, SSID, `192.168.4.1`, and whether login is required.

### 6.3 Client behavior

- Captive-portal detection routes may show a small login/launch page.
- The full dashboard must remain directly available at `http://192.168.4.1` even when captive-portal detection fails.
- All application assets must be served by the ESP32; no fonts, scripts, APIs, or images may require internet access.
- The dashboard must clearly distinguish station mode, maintenance AP mode, scheduled sleep, and disconnected state.

## 7. Measurement calibration and validation

### 7.1 Calibration modes

#### Container-depth compatibility mode

The existing calculation remains:

```text
waterDepthCm = max(containerDepthCm - distanceCm, 0)
waterPercent = clamp(waterDepthCm / containerDepthCm * 100, 0, 100)
```

#### Full/empty calibration mode

The preferred field calibration records actual sensor distances at known full and empty conditions:

```text
waterPercent = clamp((emptyDistanceCm - distanceCm) /
                     (emptyDistanceCm - fullDistanceCm) * 100, 0, 100)
```

Validation requires:

- `fullDistanceCm >= minimumValidDistanceCm`,
- `emptyDistanceCm <= maximumValidDistanceCm`,
- `emptyDistanceCm > fullDistanceCm`,
- at least 5 cm between full and empty calibration points.

The dashboard provides separate **Capture full point** and **Capture empty point** actions. Each capture uses a stable filtered value and asks for confirmation before saving.

### 7.2 Physical range validation

- Measurements below `minimumValidDistanceCm` are `too_close`, not `100%`.
- Measurements above `maximumValidDistanceCm` or pulse timeouts are `out_of_range`.
- The default valid range is 20–450 cm and is adjustable only within hardware-safe limits documented for the installed sensor.
- A reading outside the calibrated full/empty span but inside the physical sensor range may be clamped for percentage display, but it includes an `outsideCalibrationRange: true` diagnostic flag.
- Raw duration and unfiltered distance remain available in diagnostics.

### 7.3 Filtering and spike rejection

- `medianWindowSize` must be an odd integer from 3 through 15.
- Only physically valid samples enter the median window.
- A timeout or invalid sample must not cause an older valid reading to receive a new timestamp.
- If a new filtered distance differs from the last accepted distance by more than `maximumStepCm`, it is marked `pending_confirmation`.
- A large change is accepted only after `stepConfirmationSamples` consecutive filtered readings support the new level.
- `maximumStepCm: 0` disables step rejection.
- After `invalidSamplesBeforeFault` consecutive invalid samples, the published state becomes a sensor fault and the last good value is labeled stale rather than current.

### 7.4 Reading states

The reading contract adds the following states:

- `ok`
- `pending_confirmation`
- `too_close`
- `out_of_range`
- `invalid`
- `stale`
- `no_data`

The API also reports:

```json
{
  "distanceCm": 42.3,
  "rawDistanceCm": 43.1,
  "waterPercent": 64.8,
  "readingState": "ok",
  "outsideCalibrationRange": false,
  "consecutiveInvalidSamples": 0,
  "sampleSequence": 1842,
  "timestamp": "2026-07-14T18:30:00Z",
  "uptimeMilliseconds": 928400
}
```

`sampleSequence` and monotonic uptime allow freshness checks when internet time is unavailable. `timestamp` is `null` until the clock has been synchronized.

### 7.5 Installation requirements

Deployment documentation must require:

- a level, downward-facing sensor,
- clearance from walls, pipes, ladders, and fill streams,
- full and empty points outside the sensor blind zone,
- evaluation of foam, condensation, turbulence, and acoustic reflections,
- a stilling tube or alternate sensor where surface conditions make ultrasonic readings unreliable,
- verification that the sensor ECHO signal is safely level-shifted for ESP32 GPIO,
- a common electrical ground and documented supply decoupling.

This release measures linear liquid height. Tanks with irregular cross-sections require a later geometry/volume specification before the displayed percentage can be called volume remaining.

## 8. Reliability and recovery

### 8.1 Watchdogs

- Enable the ESP32 task watchdog for the main control task.
- Sensor sampling, Wi-Fi management, display updates, and request handling must be non-blocking or bounded so the watchdog remains meaningful.
- No individual sensor operation may block longer than its documented echo timeout.
- A watchdog restart records a reboot reason and increments a persistent, wear-managed counter.
- The device must return to sensing without user action after a watchdog or brownout reset.

### 8.2 Freshness monitoring

- A reading becomes stale when no accepted sample has been produced within the greater of three configured sampling intervals or five seconds.
- Scheduled sleep is not treated as a stale-reading fault while the device is intentionally asleep; the last reading is labeled with its age and sleep state.
- Status messages distinguish sensor failure, intentional sleep, network loss, and clock-unsynchronized state.
- WebSocket clients receive a status transition when a reading becomes stale, even if no new sensor reading is available.

### 8.3 Diagnostics

Add authenticated `GET /api/diagnostics` with:

- firmware version and build identifier,
- uptime and reset reason,
- current and minimum free heap,
- watchdog and brownout reset counts,
- total samples, accepted samples, timeouts, rejected spikes, and consecutive failures,
- last accepted sample uptime and timestamp when available,
- Wi-Fi reconnect count and signal strength,
- configuration schema version,
- flash/application size and available update-space indicator,
- battery voltage/state when enabled.

Secrets, password hashes, session tokens, and Wi-Fi passwords must never appear in diagnostics.

Counters with high update frequency remain in RAM. Persistent reset counters use wear-aware storage and are written only at boot or another bounded event.

### 8.4 Fault containment

- OLED initialization failure must not stop sensing or networking.
- NTP failure must not invalidate an otherwise valid measurement.
- Network scans and slow clients must not stop sampling.
- A malformed request receives an error without restarting the device.
- Failure to save configuration leaves the previous validated configuration active.
- Memory allocation or web-service failures are reported where possible and trigger controlled recovery if normal operation cannot continue.

### 8.5 Firmware capacity

- Release builds must document flash and static RAM use.
- The selected partition must leave at least 10% of the application partition free after embedding dashboard assets, or the partition/design must be revised before release.
- Runtime soak testing must monitor minimum free heap and reject a release with sustained memory loss.

## 9. Authentication and local security

### 9.1 Threat model

The device remains a local-network product. This specification protects against casual or unauthorized access by people within Wi-Fi range or on the same LAN. It does not make the ESP32 safe to expose directly to the public internet.

### 9.2 Initial setup

- The initial release may use one generic bootstrap setup credential shared by newly prepared devices.
- The generic bootstrap credential is a transitional commissioning mechanism, not a permanent administrator credential.
- The initial provisioning page requires the bootstrap credential before accepting Wi-Fi credentials or creating the administrator password.
- Setup requires creation of an administrator password before the device exits first-run mode.
- Completing setup disables the bootstrap credential on that device. It cannot be used to log in, change configuration, or repeat provisioning afterward.
- Administrator passwords must be at least 10 characters. The UI recommends a longer passphrase.
- There is no universal default administrator password.

The bootstrap credential must be configurable at build or device-preparation time and must not be printed in normal serial output, returned by an API, or shown by the device after setup. Deployment documentation must state that all uncommissioned devices share this initial credential and therefore must be commissioned in a controlled setting. A later release should replace it with a unique per-device secret or documented physical-presence setup action.

### 9.3 Credential storage

- Store a salted password verifier produced by a memory-appropriate password derivation function available on the target platform; never store the administrator password in plaintext.
- Wi-Fi passwords remain write-only and are stored using the ESP32's protected non-volatile facilities where available.
- Passwords, verifiers, setup secrets, and tokens never appear in serial logs, API responses, WebSocket events, or diagnostics.
- A password change invalidates all existing sessions.

The exact password derivation algorithm and cost must be selected and benchmarked during implementation review; it must not starve sensor sampling or trigger the watchdog.

### 9.4 Sessions and authorization

- Successful login creates a random, time-limited session token.
- Tokens are accepted only through an `HttpOnly`, `SameSite=Strict` cookie.
- Session lifetime defaults to 30 minutes of inactivity and is capped at 12 hours.
- Reading, status, and dashboard access may be configured as either authenticated-only or read-only guest access. The default is authenticated-only.
- Configuration, diagnostics, display wake, Wi-Fi changes, credential changes, and reset operations always require an administrator session.
- WebSocket connections require a valid session and are closed when that session expires.

### 9.5 Request protections

- State-changing requests require JSON, an authenticated session, and a per-session anti-CSRF token or equivalent verified request header.
- The firmware validates the `Origin` header when present and accepts only its own station and AP origins.
- Login failures use bounded exponential delay. Five failures within ten minutes lock login attempts from that client for fifteen minutes while local sensing continues.
- Request bodies, header sizes, concurrent clients, and authentication work queues have explicit bounds.
- Error responses do not reveal whether a guessed password was close or whether a session token once existed.

### 9.6 Maintenance AP protection

- The maintenance AP uses WPA2 or stronger support available on the selected ESP32 platform.
- Before first-run setup, the maintenance AP may use the approved generic bootstrap credential.
- Completing setup derives or generates a device-specific maintenance AP password; the generic bootstrap credential must no longer grant AP access to that device.
- The post-setup maintenance AP password is not the administrator password.
- The AP SSID may retain the unique `WaterLevel-XXXX` suffix for identification.
- AP credentials can be rotated by an administrator.
- The AP must not bridge or route clients to the station network.
- Captive DNS is active only while the maintenance AP is active.

### 9.7 Recovery and factory reset

- Forgotten administrator credentials require deliberate physical presence: a documented hardware-button sequence or USB serial recovery procedure.
- Network reset and full factory reset are separate operations.
- Network reset removes Wi-Fi credentials but retains calibration and administrator credentials.
- Factory reset removes Wi-Fi credentials, administrator credentials, sessions, calibration, and device configuration, then returns to initial setup.
- A dashboard or API factory reset requires reauthentication plus an explicit confirmation value.
- The existing unauthenticated `WIFI_RESET` serial command must be replaced by a physical-presence recovery procedure before release. A serial port alone must not silently bypass administration credentials unless the deployment explicitly treats physical USB access as trusted and documents that decision.

### 9.8 Transport limitations

Local HTTP traffic is not encrypted. WPA2 protects radio traffic only between a client and its access point; it does not replace HTTPS on a shared LAN. The dashboard must display this limitation in administration help. Public internet exposure and router port forwarding remain prohibited.

## 10. API additions and changes

### 10.1 New endpoints

- `POST /api/auth/login`
- `POST /api/auth/logout`
- `GET /api/auth/session`
- `PUT /api/auth/password`
- `GET /api/diagnostics`
- `POST /api/display/wake`
- `POST /api/calibration/capture-full`
- `POST /api/calibration/capture-empty`
- `POST /api/network/reset`
- `POST /api/factory-reset`

All endpoints except initial login/setup require authorization according to Section 9.4.

### 10.2 Existing endpoints

- `GET /api/status` adds sleep, freshness, battery, authentication-required, and maintenance-AP state.
- `GET /api/reading` uses the expanded reading contract in Section 7.4.
- `GET /api/config` and `PUT /api/config` use the versioned model in Section 4.
- `/ws` requires an authorized session and publishes status changes for stale readings, sleep transitions, battery warnings, and recoveries.
- Setup endpoints are accessible only in first-run mode or to an authenticated administrator.

### 10.3 Configuration schema version

The stored and API configuration includes `schemaVersion`. This release uses version `2`. Migration from the current unversioned layout is treated as version `1` and must be covered by an automated or hardware-backed migration test.

## 11. Dashboard requirements

The dashboard adds:

- a login screen that works without internet,
- a clear station/AP/sleep mode indicator,
- configuration controls with power-impact explanations,
- a countdown or next-wake explanation for scheduled sleep,
- guided full/empty calibration with live stability feedback,
- distinct stale, too-close, out-of-range, pending-change, and sensor-fault messages,
- battery voltage and threshold state only when monitoring is enabled,
- authenticated diagnostics and reset controls,
- password and AP credential management,
- warnings before enabling settings that reduce availability.

The UI must remain usable on a current mobile browser at `192.168.4.1` without DNS, internet access, external assets, or an installed application.

## 12. Deployment documentation requirements

Before release, documentation must include:

- a commissioning checklist,
- full/empty calibration procedure,
- supported sensor range and blind-zone warning,
- stilling-tube and problematic-surface guidance,
- safe ECHO level-shifting requirement,
- enclosure, grounding, power, and surge recommendations,
- how to identify and join the maintenance AP,
- administrator/AP credential handoff procedure,
- scheduled-sleep behavior and expected unreachability,
- network reset and factory-reset procedures,
- interpretation of diagnostics and reboot reasons,
- an explicit prohibition on public port forwarding.

## 13. Acceptance criteria

This release is complete only when all applicable criteria pass:

1. An upgraded device with old settings boots in continuous mode and preserves its existing configuration.
2. All new power-saving behavior remains disabled unless explicitly enabled.
3. Sampling intervals from 0.5 seconds through 1 hour work and freshness thresholds follow the selected interval.
4. OLED timeout does not interrupt sensing, serial, Wi-Fi, or web operation.
5. Scheduled sleep follows the configured wake/sample/awake/sleep cycle for 100 consecutive cycles.
6. A device without battery-monitoring hardware never enables or fabricates battery data.
7. With the router absent, the protected maintenance AP appears within the configured delay.
8. A phone can log in at `192.168.4.1` and use the complete monitoring dashboard without internet.
9. Station reconnection succeeds while the AP is active and does not lose saved calibration.
10. Full/empty calibration rejects reversed, too-close, too-far, and insufficient-span values.
11. Physical-range failures never produce a valid percentage.
12. A single large spike is rejected; a sustained large change is accepted after the configured confirmation count.
13. Invalid samples do not refresh the last-good timestamp.
14. Missing accepted samples produce a stale status within the defined interval.
15. NTP failure leaves timestamp `null` while uptime and sample sequence continue to show freshness.
16. OLED failure, Wi-Fi outage, network scan, malformed requests, and slow clients do not stop sampling.
17. A forced task stall causes watchdog recovery and records the correct reboot reason.
18. Brownout or abrupt power-cycle testing preserves the last fully committed configuration.
19. No password, verifier, setup secret, or token appears in API, WebSocket, serial, or diagnostic output.
20. Default device access requires login; every state-changing endpoint rejects unauthenticated and cross-origin requests.
21. The generic bootstrap credential works only before commissioning; afterward, the maintenance AP uses a device-specific credential that can be rotated.
22. Login throttling limits repeated guesses without blocking sensing or an already authenticated local display.
23. Network reset retains calibration and administrator credentials; factory reset clears all user state.
24. A 24-hour multi-client test and a seven-day unattended soak test complete without stale sampling, crash loops, or sustained heap loss.
25. The release build retains at least 10% application-partition headroom.
26. All dashboard assets load successfully with upstream internet physically disconnected.

## 14. Implementation sequence after approval

1. Version and migrate the persistent configuration schema.
2. Refactor sampling and freshness tracking around monotonic time.
3. Add calibration, physical validation, filtering, and diagnostic counters.
4. Add watchdogs, reset diagnostics, and fault-containment tests.
5. Add authentication, sessions, protected endpoints, and unique maintenance AP credentials.
6. Serve the complete dashboard through the maintenance AP and revise provisioning.
7. Add optional power controls, scheduled sleep, and optional battery monitoring.
8. Update the dashboard and deployment documentation.
9. Run migration, security, power-cycle, multi-client, and soak-test acceptance suites.

Approved review decisions:

- Initial credential provisioning may use a generic bootstrap credential, subject to the forced replacement and commissioning restrictions in Section 9.
- The scheduled-sleep awake window defaults to 30 seconds.

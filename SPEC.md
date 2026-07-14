# Water Level Sensor Application Spec

## 1. Purpose

Build a water level monitoring application for an ESP32-based ultrasonic sensor system.

The system must:
- read distance measurements from the sensor,
- convert those measurements into water remaining percentage,
- support both Wi-Fi and USB connectivity,
- provide a modern browser-based GUI,
- allow the user to configure the container depth in centimeters.

This project follows spec-driven development. Implementation should conform to this document unless the spec is revised first.

## 2. Product Goals

- Provide a live view of current water level as a percentage.
- Support direct network access when the ESP32 is on Wi-Fi.
- Support local USB access when the ESP32 is attached to a computer.
- Keep one shared frontend for both transport modes.
- Make configuration explicit and user-controlled.

## 3. Non-Goals For V1

- Multi-user authentication
- Cloud sync
- Historical analytics beyond a basic recent reading list
- Mobile app store packaging
- Advanced tank geometry beyond a simple vertical depth model

## 4. System Overview

The project has three parts:

1. `firmware/`
   ESP32 firmware reads the ultrasonic sensor and exposes readings over:
   - serial over USB
   - HTTP and WebSocket over Wi-Fi

2. `dashboard/`
   Browser UI that shows current level, sensor status, and configuration.

3. `bridge/`
   Local service for USB mode. It reads ESP32 serial output and exposes the same API shape as the Wi-Fi firmware so the dashboard does not need a separate code path.

## 5. Chosen Technical Direction

### 5.1 Frontend

- React
- Vite
- TypeScript

Reason:
- modern browser UX,
- strong local development experience,
- easy reuse for desktop and mobile browsers,
- clean fit for live WebSocket updates.

### 5.2 USB Bridge

- Node.js
- TypeScript

Reason:
- natural fit with the frontend stack,
- mature serial libraries,
- simple local HTTP and WebSocket bridge.

### 5.3 Firmware

- ESP32 using Arduino framework

Reason:
- aligns with the existing handoff document,
- lowest friction for current hardware setup,
- familiar library ecosystem for ESP32 and SSD1306.

## 6. Core User Flows

### 6.1 First Run

1. User powers the ESP32.
2. User opens the dashboard.
3. User chooses connection mode:
   - Wi-Fi device
   - Local USB bridge
4. User opens config and sets container depth in cm.
5. Dashboard begins showing live water percentage and raw distance.

### 6.2 Wi-Fi Mode

1. Dashboard connects to the ESP32 over the network.
2. Dashboard requests current config and status.
3. Dashboard subscribes to live readings over WebSocket.

### 6.3 USB Mode

1. User starts the local bridge on the PC.
2. Bridge reads sensor messages from serial.
3. Dashboard connects to the bridge as if it were the device API.

## 7. Water Level Calculation

### 7.1 Inputs

- `containerDepthCm`
  The full measurable depth of the container from sensor reference point to empty-bottom reference point.

- `distanceCm`
  The current distance measured by the ultrasonic sensor from the sensor face to the water surface.

### 7.2 Derived Values

- `waterDepthCm = containerDepthCm - distanceCm`
- `waterPercent = clamp((waterDepthCm / containerDepthCm) * 100, 0, 100)`

### 7.3 Rules

- If `containerDepthCm <= 0`, the reading is invalid.
- If `distanceCm < 0`, the reading is invalid.
- If `distanceCm > containerDepthCm`, treat water depth as `0`.
- If the sensor reports timeout or out-of-range, the dashboard shows `No Reading` instead of a percentage.

### 7.4 Assumption

V1 assumes the sensor is mounted above the water and points vertically downward. The container is treated as a simple linear depth measurement.

## 8. Dashboard Requirements

### 8.1 Main Page

The main page must show:
- water remaining percentage,
- current water depth in cm,
- current measured sensor distance in cm,
- connection mode,
- device status,
- last update time,
- alert state when readings are invalid or out of range.

### 8.2 Config Panel

The config panel must allow the user to set:
- `containerDepthCm`
- optional display name for the container
- warning threshold percentage
- critical threshold percentage
- preferred connection mode

For V1, only `containerDepthCm` is mandatory.

### 8.3 Visual Behavior

- Large percent readout on the main page
- Secondary raw measurement details below
- Clear invalid-state messaging
- Status badge for `connected`, `disconnected`, `reading`, `error`

## 9. Connectivity Contract

The dashboard should talk to either the firmware or the bridge using the same API shape.

### 9.1 REST Endpoints

`GET /api/status`
- returns connection and device status

`GET /api/config`
- returns current configuration

`PUT /api/config`
- updates configuration values

`GET /api/reading`
- returns latest reading snapshot

### 9.2 WebSocket

`/ws`

The server sends:
- `status` events
- `reading` events
- `config` events

## 10. Data Shapes

### 10.1 Config

```json
{
  "containerDepthCm": 120,
  "containerName": "Rain Barrel",
  "warningThresholdPercent": 35,
  "criticalThresholdPercent": 15,
  "preferredMode": "wifi"
}
```

### 10.2 Reading

```json
{
  "distanceCm": 42.3,
  "waterDepthCm": 77.7,
  "waterPercent": 64.8,
  "rawDurationUs": 2488,
  "readingState": "ok",
  "timestamp": "2026-07-13T22:30:00Z"
}
```

### 10.3 Status

```json
{
  "mode": "wifi",
  "deviceConnected": true,
  "sensorHealthy": true,
  "firmwareVersion": "0.1.0",
  "serialPort": null,
  "ipAddress": "192.168.1.40"
}
```

## 11. Serial Message Contract

USB mode needs a stable serial format.

V1 serial output should be JSON lines:

```json
{"distanceCm":42.3,"rawDurationUs":2488,"readingState":"ok","timestamp":"2026-07-13T22:30:00Z"}
```

Requirements:
- one JSON object per line,
- UTF-8 text,
- newline terminated,
- 115200 baud.

This replaces fragile human-readable strings like `Distance: 42.3 cm` for application integration.

## 12. Firmware Requirements

- Sample the sensor on a fixed interval.
- Preserve serial output for USB mode.
- Expose latest reading and config over HTTP.
- Push live updates over WebSocket.
- Store config locally if feasible; otherwise accept runtime-only config for initial development.

## 13. Error Handling

The system must distinguish:
- no device connected
- serial connection failure
- Wi-Fi unreachable
- invalid sensor reading
- out-of-range reading
- missing container depth configuration

The dashboard should not display a fake percentage when no valid reading exists.

## 14. Repository Layout

```text
Water Level Sensor/
  SPEC.md
  README.md
  docs/
  firmware/
  dashboard/
  bridge/
```

## 15. Milestones

### Milestone 1
- finalize spec
- install local tooling
- scaffold dashboard and bridge

### Milestone 2
- define and test serial JSON output from ESP32
- implement local USB bridge
- implement mock-data dashboard

### Milestone 3
- add Wi-Fi API and WebSocket to firmware
- connect dashboard to live device

### Milestone 4
- refine UI, config persistence, and alert handling

## 16. Open Decisions

- Whether config persistence lives on the ESP32, the bridge, or both
- Whether Wi-Fi mode starts as station mode only or supports captive setup
- Whether the firmware keeps the OLED logic from the original handoff or the GUI replaces it as the primary interface

## 17. Immediate Next Step

Install the local development tooling:
- Node.js and npm
- Arduino IDE or arduino-cli

After that, scaffold:
- `dashboard/` with Vite React TypeScript
- `bridge/` with Node TypeScript
- `firmware/` with an ESP32 sketch that emits JSON readings

# ESP32 Water Level Monitor

A live water-level dashboard for an ESP32, JSN-SR04T ultrasonic sensor, and SSD1306 OLED. The ESP32 can host the dashboard directly over local Wi-Fi or stream measurements over USB serial to the optional Node.js bridge.

## What works

- Live USB readings at 115200 baud
- Water depth and percentage calculation
- Responsive browser dashboard with WebSocket updates
- Configurable container depth, name, and alert thresholds
- Standalone Wi-Fi dashboard, REST API, and live WebSocket readings
- Captive first-run Wi-Fi setup with persistent credentials and recovery
- Windows, macOS, and Linux development setup
- Firmware compile/upload through Arduino IDE or Arduino CLI

For first-time Wi-Fi setup, briefly join the temporary `WaterLevel-XXXX` network and choose the same 2.4 GHz Wi-Fi used by your phone or computer. The setup network has no internet access and closes automatically after setup. Rejoin your normal Wi-Fi, then open `http://water-level.local` or the IP address shown on the setup page and OLED. The complete standalone design is documented in [`docs/WIFI_MODE_SPEC.md`](docs/WIFI_MODE_SPEC.md).

The same firmware can be flashed onto additional sensors; Wi-Fi credentials and hostnames are configured separately on each device and are never compiled into the repository. See the [sensor deployment guide](docs/SENSOR_DEPLOYMENT.md) for multi-sensor setup, network changes, and behavior away from home.

## Hardware

- ESP32 Dev Module (the tested board uses a Silicon Labs CP210x USB interface)
- JSN-SR04T waterproof ultrasonic sensor
- SSD1306 128×64 I2C OLED at address `0x3C`
- Data-capable USB cable

The firmware currently expects the ultrasonic trigger on GPIO 5 and echo on GPIO 18. See the separate hardware wiring diagram for full wiring details.

## Software prerequisites

Install these before cloning the project:

1. [Git](https://git-scm.com/downloads)
2. [Node.js 22 LTS](https://nodejs.org/) (Node 20 or newer should work)
3. Either [Arduino IDE 2](https://www.arduino.cc/en/software) or [Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/)

Confirm the first two are available:

```text
git --version
node --version
npm --version
```

## Quick start

Clone the repository and enter it:

```bash
git clone https://github.com/jvangore31/Water-Level-Sensor.git
cd Water-Level-Sensor
```

Install the dashboard and bridge dependencies with the cross-platform setup command:

```bash
npm run setup
```

Connect the programmed ESP32 by USB, then start the bridge and dashboard together:

```bash
npm start
```

Open [http://localhost:5173](http://localhost:5173). Select the ESP32 serial port and choose **Connect USB** if it is not already connected.

To stop both services, press `Ctrl+C` in the terminal.

This starts the optional USB workflow. In standalone Wi-Fi mode, open the ESP32's `.local` name or LAN IP directly; `npm start` and a continuously running PC are not required.

## Upload the firmware

### Option A: Arduino IDE (recommended for first-time users)

1. Open Arduino IDE.
2. Open **File → Preferences** and add this Additional Boards Manager URL:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Open **Tools → Board → Boards Manager**, search for `esp32`, and install **esp32 by Espressif Systems**.
4. Open **Library Manager** and install:
   - `Adafruit GFX Library`
   - `Adafruit SSD1306`
   - `ArduinoJson`
   - `Async TCP`
   - `ESP Async WebServer`
5. Open `firmware/water_level_sensor/water_level_sensor.ino`.
6. Choose **Tools → Board → ESP32 Arduino → ESP32 Dev Module**.
7. Choose the ESP32 under **Tools → Port**.
8. Click **Upload**.

### Option B: Arduino CLI

Install the platform and libraries once:

```bash
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install "Adafruit GFX Library" "Adafruit SSD1306" "ArduinoJson" "Async TCP" "ESP Async WebServer"
```

Find the board port:

```bash
arduino-cli board list
```

Compile and upload, replacing `YOUR_PORT` with the listed port:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/water_level_sensor
arduino-cli upload --fqbn esp32:esp32:esp32 --port YOUR_PORT firmware/water_level_sensor
```

Port examples are `COM4` on Windows, `/dev/cu.usbserial-0001` on macOS, and `/dev/ttyUSB0` on Linux.

## Platform-specific USB setup

### Windows 10/11

- Open **Device Manager → Ports (COM & LPT)** to find the ESP32 COM port.
- If no port appears, install the [Silicon Labs CP210x VCP driver](https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers) and reconnect the board.
- Close Arduino Serial Monitor before connecting from the dashboard; only one program can own the port.
- If PowerShell blocks local scripts, the project still works because its commands use Node directly and do not require a `.ps1` script.

### macOS

- Ports normally appear as `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART`.
- Current macOS versions often recognize CP210x devices automatically. If no port appears, install the Silicon Labs driver linked above and approve it under **System Settings → Privacy & Security**.
- Close Arduino Serial Monitor before starting the bridge.

### Linux

Add your user to the serial-port group (Ubuntu, Debian, Raspberry Pi OS, and many related distributions):

```bash
sudo usermod -aG dialout "$USER"
```

Log out and back in, then verify `dialout` appears in the output of `groups`. On Arch-based systems the group can be `uucp`; use `ls -l /dev/ttyUSB0` to see the owning group.

If `ModemManager` repeatedly grabs or resets the port, stop it for testing or configure it to ignore this device.

## Project commands

Run these from the repository root:

| Command | Purpose |
| --- | --- |
| `npm run setup` | Install exact bridge and dashboard dependencies |
| `npm start` | Run the USB bridge and dashboard together |
| `npm run start:bridge` | Run only the bridge on port 8787 |
| `npm run start:dashboard` | Run only the dashboard on port 5173 |
| `npm test` | Typecheck, lint, and production-build the application |
| `npm run build` | Build the dashboard for production |
| `npm run build:firmware-assets` | Rebuild and embed the production dashboard in the firmware |

The bridge API is available at `http://localhost:8787`. The dashboard automatically uses port 8787 on the same host from which it was loaded.

## Configuration and calculation

Set the measurable container depth in the dashboard. The bridge calculates:

```text
water depth = container depth - measured sensor distance
water percent = clamp(water depth / container depth × 100, 0, 100)
```

Runtime configuration is saved locally in `bridge/.data/config.json`. That file is intentionally ignored by Git.

## Troubleshooting

### The serial-port list is empty

1. Confirm the USB cable supports data; many charging cables do not.
2. Disconnect and reconnect the ESP32.
3. Run `arduino-cli board list` or check Device Manager/System Information.
4. Install the CP210x driver if the operating system does not create a port.
5. On Linux, confirm serial-group membership as described above.

### The port is busy or access is denied

Close Arduino Serial Monitor, PlatformIO, PuTTY, screen, and any other program using the port. On Linux, also verify the port’s group permissions.

### Connected, but there are no readings

- Upload the firmware from this repository.
- Confirm the baud rate is `115200`.
- Open a serial monitor temporarily and look for one JSON object per line.
- If the OLED cannot initialize, verify that its I2C address is `0x3C`. The firmware reports the display fault over serial and continues sensor and network operation.

### Dashboard says the API is offline

Make sure `npm start` is still running and port 8787 is not blocked by a firewall. If the dashboard is on another device, enter the bridge computer’s LAN address (for example `http://192.168.1.20:8787`) in **Device API URL**. You may need to allow Node.js through the host firewall.

### Connecting to the sensor Wi-Fi removes internet access

`WaterLevel-XXXX` is only the temporary setup network and intentionally has no internet route. Use it to give the sensor your normal 2.4 GHz Wi-Fi credentials. It closes within 15 seconds after a successful connection. Reconnect the PC or phone to the selected normal Wi-Fi; both devices then keep normal router internet access and exchange sensor data locally.

If `WaterLevel-XXXX` returns after ten minutes, the saved network could not be reached. Confirm the router is online, that the 2.4 GHz network is available, and that the client-isolation or guest-network setting is disabled.

## Repository layout

```text
bridge/       Node.js USB serial bridge, REST API, and WebSocket server
dashboard/    React/Vite browser dashboard
firmware/     ESP32 Arduino sketch
docs/         Additional setup and project notes
SPEC.md       Product and protocol specification
```

Deployment procedures for additional devices and new locations are in [docs/SENSOR_DEPLOYMENT.md](docs/SENSOR_DEPLOYMENT.md).

## Development status

The USB workflow is working end to end, and standalone Wi-Fi support is in implementation. Automated application checks cover the bridge and dashboard; final captive-portal, reconnect, and long-duration acceptance checks require the ESP32 hardware. See [SPEC.md](SPEC.md) and [docs/WIFI_MODE_SPEC.md](docs/WIFI_MODE_SPEC.md) for the contracts. Contributions are welcome; read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## License

Licensed under the MIT License. See [LICENSE](LICENSE).

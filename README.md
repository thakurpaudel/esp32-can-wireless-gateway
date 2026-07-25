# ESP32 CAN Wireless Gateway

A public, extensible ESP-IDF gateway that receives Classical CAN frames through
the ESP32 TWAI controller and forwards them over both Wi-Fi and Bluetooth Low
Energy. It includes a responsive browser dashboard served directly by the ESP32
and a Python desktop monitor for computers on the same network.

## Features

- ESP-IDF project for ESP32-family chips with a TWAI/CAN controller
- Configurable CAN RX/TX pins and 125/250/500/1000 kbit/s bitrate
- Wi-Fi station mode with IP address logging and `esp32-can.local` mDNS
- Fallback access point at `http://192.168.4.1`
- Embedded real-time WebSocket dashboard with filtering and CSV export
- Runtime CAN bitrate selection from the browser dashboard
- Queued CAN transmission for standard, extended, data, and RTR frames
- BLE GATT notification stream using the same JSON format
- Full-screen Python/Tk BLE monitor and CAN transmitter with CSV export
- Standard and extended 11/29-bit CAN IDs and RTR metadata

> This project receives Classical CAN frames. It does not implement CAN FD,
> because the ESP32 TWAI peripheral is a Classical CAN controller.

## Repository layout

```text
.
├── main/
│   ├── app_main.c          Application composition
│   ├── can_gateway.c       TWAI driver and frame normalization
│   ├── wifi_manager.c      Station, fallback AP, IP, and mDNS
│   ├── web_server.c        HTTP and WebSocket transport
│   ├── ble_gateway.c       NimBLE GATT notification transport
│   ├── include/            Public component interfaces
│   └── web/index.html      Dashboard embedded in firmware
├── pc_app/
│   ├── can_monitor.py      Python desktop monitor
│   └── requirements.txt
└── docs/ARCHITECTURE.md
```

## Supported targets

The firmware is structured for ESP32, ESP32-S3, ESP32-C3, ESP32-C6, and other
ESP-IDF targets that provide TWAI. Bluetooth availability varies by chip.
ESP32-S2 has Wi-Fi but no Bluetooth, so use its Wi-Fi transport with NimBLE
disabled. Check the selected chip's datasheet before assigning GPIOs.

## Hardware

You need:

- a supported ESP32 development board;
- at least 4 MiB of flash;
- a 3.3 V CAN transceiver, such as SN65HVD230;
- a correctly terminated CAN bus and a common ground.

Default wiring:

| ESP32 | CAN transceiver |
|---|---|
| GPIO 5 (`TWAI_TX`) | TXD |
| GPIO 4 (`TWAI_RX`) | RXD |
| 3V3 | VCC |
| GND | GND |

Connect the transceiver CANH/CANL pins to the CAN bus. Never connect ESP32 GPIOs
directly to CANH or CANL.

## Build and flash

Install and activate [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).
ESP-IDF 5.2 or newer is recommended.

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

In `CAN Wireless Gateway`, configure:

1. CAN RX and TX pins for your board.
2. CAN bus bitrate.
3. Wi-Fi SSID and password.
4. Hostname and fallback access-point password.

Do not commit `sdkconfig`, because it can contain the Wi-Fi password.

## Open the web dashboard

The serial monitor prints the assigned address:

```text
Dashboard: http://192.168.1.42 or http://esp32-can.local
```

Open either address from a device connected to the same Wi-Fi network. If the
configured network is unavailable, join the `esp32-can-setup` access point and
open `http://192.168.4.1`.

Use the dashboard's **CAN bitrate** selector to switch between 125, 250, 500,
and 1000 kbit/s while the firmware is running. The TWAI driver restarts safely
with the selected timing; no reflashing is required.

The full-width **Transmit CAN frame** panel accepts an 11-bit or 29-bit CAN ID,
up to eight hexadecimal data bytes, and optional RTR mode. Transmit requests
enter a dedicated FreeRTOS queue and are processed by the task that owns the
TWAI driver. Successfully queued TX frames appear in the monitor with `TX`;
received bus frames appear with `RX`.

For periodic traffic, set a repeat interval from 20 to 60000 milliseconds and
select **Start continuous send**. The dashboard waits for each HTTP request to
complete before scheduling the next one, preventing overlapping requests.
Select **Stop continuous send** before changing the frame. The compact controls
remain fixed while only the large CAN frame table scrolls on desktop screens.

The ESP32 reports TWAI transmit acceptance or failure back to the dashboard over
WebSocket. Failed sends display a `Can't send` message and stop continuous
transmission. A two-second HTTP heartbeat changes the badge from **Live** to
**Disconnected** if the ESP32 or network connection is lost.

## Run the Python BLE desktop monitor

```bash
cd pc_app
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
python3 can_monitor.py --device ESP32-CAN
```

The application scans for the ESP32 directly over Bluetooth Low Energy. It does
not need Wi-Fi, an IP address, or the browser dashboard. On macOS, allow your
terminal application to use Bluetooth when prompted. The application starts as
a normal resizable window; select **Full screen** when needed and press `Escape`
to return to windowed mode.

If Homebrew Python reports that `_tkinter` is missing:

```bash
brew install python-tk@3.14
```

## BLE interface

- Device name: `ESP32-CAN`
- Service UUID: `11001198-240f-45b2-a245-3aea204f9b10`
- Frame characteristic UUID: `12001198-240f-45b2-a245-3aea204f9b10`
- Command characteristic UUID: `13001198-240f-45b2-a245-3aea204f9b10`
- Frame characteristic property: notify
- Command characteristic property: write with response

Subscribe to the frame characteristic to receive compact 19-byte CAN packets.
Write compact commands to transmit frames or change bitrate. See
[architecture and BLE wire format](docs/ARCHITECTURE.md).

The versioned service UUID prevents macOS CoreBluetooth from reusing an older
cached GATT layout after new characteristics are added. If the application
reports a protocol mismatch, update and reflash the ESP32 firmware before
reconnecting.

## Safety and performance

This reference gateway is intended for monitoring and development. Validate bus
loading, error handling, electrical isolation, security, and real-time behavior
before using it in a vehicle or safety-critical system. Wi-Fi and BLE delivery
are best-effort and must not be part of a safety control loop.

The HTTP interface has no authentication. Any device that can reach the ESP32
on the local network can request a CAN transmission. Use it only on a trusted,
isolated development network and never expose the gateway directly to the
internet.

## License

MIT © 2026 Thakur Paudel

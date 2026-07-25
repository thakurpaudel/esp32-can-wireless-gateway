# Architecture

```text
CAN bus
   │ RX / TX
CAN transceiver (required)
   │ RX/TX
ESP32 TWAI driver → normalized gateway_frame_t
                           ├─ HTTP WebSocket → browser dashboard
                           └─ NimBLE GATT notify → BLE client

PC desktop monitor ── BLE notify/write ── ESP32
Browser TX panel ── HTTP API ── TX queue ── TWAI driver ── CAN bus
```

## Design boundaries

- `can_gateway` owns the TWAI peripheral and converts driver messages into a
  transport-neutral frame. It also owns the queued transmit path.
- `web_server` serves the embedded dashboard and broadcasts JSON frames to all
  WebSocket clients.
- `ble_gateway` exposes the same JSON frame through a notify characteristic.
- `wifi_manager` owns station connection, fallback AP, IP logging, and mDNS.
- `pc_app` connects directly over BLE and supports monitoring, transmission,
  bitrate changes, and CSV export without Wi-Fi.

The transport-neutral frame keeps CAN acquisition independent of the user
interfaces. Slow or disconnected clients do not block CAN reception.

## Wi-Fi wire format

The WebSocket transport uses one compact JSON object per frame:

```json
{"timestamp_us":1234567,"id":291,"id_hex":"0x123","extended":false,"remote":false,"direction":"rx","dlc":8,"data":[1,2,3,4,5,6,7,8]}
```

## BLE wire format

All multibyte integers are little-endian. Packets fit the default 20-byte GATT
notification payload and do not depend on a larger negotiated MTU.

Frame notifications are always 19 bytes:

| Offset | Size | Meaning |
|---|---:|---|
| 0 | 1 | Opcode `0x81` |
| 1 | 1 | Flags: bit 0 extended, bit 1 RTR, bit 2 transmitted |
| 2 | 1 | DLC, 0–8 |
| 3 | 4 | CAN ID |
| 7 | 4 | Milliseconds since boot, wrapping at 32 bits |
| 11 | 8 | Data, zero-padded |

CAN transmit commands contain opcode `0x01`, flags, DLC, a four-byte CAN ID,
and 0–8 data bytes. Bitrate commands contain opcode `0x02` followed by the
four-byte bitrate in bit/s.

## Hardware

An external 3.3 V CAN transceiver is mandatory. Connect ESP32 `TWAI_TX` and
`TWAI_RX` to the transceiver logic pins, then connect `CANH`, `CANL`, and common
ground to the bus. Terminate each end of the bus with 120 Ω. Do not connect an
ESP32 directly to CANH/CANL.

ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, and other targets differ in
Bluetooth support and available pins. ESP32-S2 has no Bluetooth, so disable
NimBLE for that target and use Wi-Fi only.

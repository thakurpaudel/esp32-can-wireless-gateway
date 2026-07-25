# Architecture

```text
CAN bus
   │
CAN transceiver (required)
   │ RX/TX
ESP32 TWAI driver → normalized gateway_frame_t
                           ├─ HTTP WebSocket → browser dashboard
                           └─ NimBLE GATT notify → BLE client

PC desktop monitor ── WebSocket ── Wi-Fi LAN ── ESP32
```

## Design boundaries

- `can_gateway` owns the TWAI peripheral and converts driver messages into a
  transport-neutral frame.
- `web_server` serves the embedded dashboard and broadcasts JSON frames to all
  WebSocket clients.
- `ble_gateway` exposes the same JSON frame through a notify characteristic.
- `wifi_manager` owns station connection, fallback AP, IP logging, and mDNS.
- `pc_app` is an independent Python consumer of the documented WebSocket format.

The transport-neutral frame keeps CAN acquisition independent of the user
interfaces. Slow or disconnected clients do not block CAN reception.

## Wire format

Both transports use one compact JSON object per frame:

```json
{"timestamp_us":1234567,"id":291,"id_hex":"0x123","extended":false,"remote":false,"dlc":8,"data":[1,2,3,4,5,6,7,8]}
```

## Hardware

An external 3.3 V CAN transceiver is mandatory. Connect ESP32 `TWAI_TX` and
`TWAI_RX` to the transceiver logic pins, then connect `CANH`, `CANL`, and common
ground to the bus. Terminate each end of the bus with 120 Ω. Do not connect an
ESP32 directly to CANH/CANL.

ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, and other targets differ in
Bluetooth support and available pins. ESP32-S2 has no Bluetooth, so disable
NimBLE for that target and use Wi-Fi only.

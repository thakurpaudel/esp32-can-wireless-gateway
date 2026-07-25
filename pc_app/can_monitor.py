#!/usr/bin/env python3
"""Full-screen BLE CAN monitor and transmitter for ESP32-CAN."""

from __future__ import annotations

import argparse
import asyncio
import csv
import queue
import struct
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "11001198-240f-45b2-a245-3aea204f9b10"
FRAME_UUID = "12001198-240f-45b2-a245-3aea204f9b10"
COMMAND_UUID = "13001198-240f-45b2-a245-3aea204f9b10"


class GatewayMonitor:
    def __init__(self, root: tk.Tk, device_name: str) -> None:
        self.root = root
        self.root.title("ESP32 CAN BLE Gateway")
        self.root.geometry("1100x700")
        self.root.minsize(850, 520)
        self.fullscreen = False

        self.device_name = tk.StringVar(value=device_name)
        self.status = tk.StringVar(value="Disconnected")
        self.tx_status = tk.StringVar(value="Ready")
        self.messages: queue.Queue[dict[str, object]] = queue.Queue()
        self.frames: list[dict[str, object]] = []
        self.stop = threading.Event()
        self.loop: asyncio.AbstractEventLoop | None = None
        self.client: BleakClient | None = None
        self.command_characteristic: object | None = None

        self._build_ui()
        self.root.bind("<Escape>", self.leave_fullscreen)
        self.root.after(50, self.drain)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _build_ui(self) -> None:
        bar = ttk.Frame(self.root, padding=12)
        bar.pack(fill=tk.X)
        ttk.Label(bar, text="BLE device:").pack(side=tk.LEFT)
        ttk.Entry(bar, textvariable=self.device_name, width=24).pack(
            side=tk.LEFT, padx=6
        )
        ttk.Button(bar, text="Connect", command=self.connect).pack(side=tk.LEFT)
        ttk.Button(bar, text="Export CSV", command=self.export).pack(
            side=tk.RIGHT, padx=4
        )
        self.fullscreen_button = ttk.Button(
            bar, text="Full screen", command=self.toggle_fullscreen
        )
        self.fullscreen_button.pack(side=tk.RIGHT, padx=4)
        ttk.Label(self.root, textvariable=self.status, padding=(12, 0)).pack(
            anchor=tk.W
        )

        transmit = ttk.LabelFrame(self.root, text="Transmit CAN frame", padding=10)
        transmit.pack(fill=tk.X, padx=12, pady=10)
        self.tx_id = tk.StringVar(value="123")
        self.tx_data = tk.StringVar(value="00 01 02 03")
        self.tx_format = tk.StringVar(value="Standard")
        self.tx_remote = tk.BooleanVar(value=False)
        self.bitrate = tk.StringVar(value="500 kbit/s")

        ttk.Label(transmit, text="CAN ID (hex)").grid(row=0, column=0, sticky=tk.W)
        ttk.Entry(transmit, textvariable=self.tx_id, width=18).grid(
            row=1, column=0, padx=(0, 8), sticky=tk.EW
        )
        ttk.Label(transmit, text="Format").grid(row=0, column=1, sticky=tk.W)
        ttk.Combobox(
            transmit,
            textvariable=self.tx_format,
            values=("Standard", "Extended"),
            state="readonly",
            width=13,
        ).grid(row=1, column=1, padx=(0, 8), sticky=tk.EW)
        ttk.Label(transmit, text="Data bytes (hex)").grid(
            row=0, column=2, sticky=tk.W
        )
        self.data_entry = ttk.Entry(transmit, textvariable=self.tx_data)
        self.data_entry.grid(row=1, column=2, padx=(0, 8), sticky=tk.EW)
        ttk.Checkbutton(
            transmit,
            text="RTR",
            variable=self.tx_remote,
            command=self.remote_changed,
        ).grid(row=1, column=3, padx=(0, 8))
        ttk.Button(transmit, text="Send", command=self.send_frame).grid(
            row=1, column=4, padx=(0, 16)
        )
        ttk.Label(transmit, text="CAN bitrate").grid(row=0, column=5, sticky=tk.W)
        bitrate = ttk.Combobox(
            transmit,
            textvariable=self.bitrate,
            values=("125 kbit/s", "250 kbit/s", "500 kbit/s", "1 Mbit/s"),
            state="readonly",
            width=13,
        )
        bitrate.grid(row=1, column=5, padx=(0, 8))
        bitrate.bind("<<ComboboxSelected>>", self.set_bitrate)
        ttk.Label(transmit, textvariable=self.tx_status).grid(
            row=1, column=6, sticky=tk.W
        )
        transmit.columnconfigure(2, weight=1)

        columns = ("time", "direction", "id", "type", "dlc", "data")
        self.table = ttk.Treeview(self.root, columns=columns, show="headings")
        widths = (130, 90, 140, 120, 60, 500)
        for name, width in zip(columns, widths):
            self.table.heading(name, text=name.upper())
            self.table.column(name, width=width, anchor=tk.W)
        scrollbar = ttk.Scrollbar(
            self.root, orient=tk.VERTICAL, command=self.table.yview
        )
        self.table.configure(yscrollcommand=scrollbar.set)
        self.table.pack(fill=tk.BOTH, expand=True, padx=(12, 28), pady=(0, 12))
        scrollbar.place(relx=1.0, rely=0.25, relheight=0.72, anchor=tk.NE)

    def toggle_fullscreen(self, _event: object | None = None) -> None:
        self.fullscreen = not self.fullscreen
        self.root.attributes("-fullscreen", self.fullscreen)
        self.fullscreen_button.configure(
            text="Exit full screen" if self.fullscreen else "Full screen"
        )

    def leave_fullscreen(self, _event: object | None = None) -> None:
        if self.fullscreen:
            self.toggle_fullscreen()

    def connect(self) -> None:
        if self.loop and self.loop.is_running():
            asyncio.run_coroutine_threadsafe(self.disconnect(), self.loop)
        self.stop.set()
        self.stop = threading.Event()
        threading.Thread(target=self.ble_thread, daemon=True).start()

    def ble_thread(self) -> None:
        asyncio.run(self.ble_session())

    async def ble_session(self) -> None:
        self.loop = asyncio.get_running_loop()
        name = self.device_name.get().strip()
        self.messages.put({"_status": f"Scanning for {name}…"})
        try:
            device = await BleakScanner.find_device_by_filter(
                lambda candidate, advertisement: (
                    candidate.name == name
                    or advertisement.local_name == name
                    or SERVICE_UUID.lower()
                    in [uuid.lower() for uuid in advertisement.service_uuids]
                ),
                timeout=15.0,
            )
            if not device:
                raise RuntimeError(f"BLE device '{name}' not found")

            async with BleakClient(device, disconnected_callback=self.disconnected) as client:
                self.client = client
                frame_characteristic = client.services.get_characteristic(FRAME_UUID)
                self.command_characteristic = client.services.get_characteristic(
                    COMMAND_UUID
                )
                if not frame_characteristic or not self.command_characteristic:
                    raise RuntimeError(
                        "BLE protocol mismatch: required RX/TX characteristics "
                        "were not found. Pull the latest code, rebuild, and reflash "
                        "the ESP32 firmware."
                    )
                await client.start_notify(frame_characteristic, self.notification)
                self.messages.put(
                    {"_status": f"Connected over BLE: {device.name or device.address}"}
                )
                while not self.stop.is_set() and client.is_connected:
                    await asyncio.sleep(0.2)
        except Exception as exc:
            self.messages.put({"_status": f"BLE error: {exc}"})
        finally:
            self.client = None
            self.command_characteristic = None

    def disconnected(self, _client: BleakClient) -> None:
        self.messages.put({"_status": "BLE disconnected"})

    def notification(self, _sender: object, payload: bytearray) -> None:
        if len(payload) != 19 or payload[0] != 0x81:
            return
        flags, dlc, can_id, timestamp_ms = struct.unpack_from("<BBII", payload, 1)
        dlc = min(dlc, 8)
        self.messages.put(
            {
                "timestamp_us": timestamp_ms * 1000,
                "id_hex": f"0x{can_id:X}",
                "extended": bool(flags & 0x01),
                "remote": bool(flags & 0x02),
                "direction": "tx" if flags & 0x04 else "rx",
                "dlc": dlc,
                "data": list(payload[11 : 11 + dlc]),
            }
        )

    def send_frame(self) -> None:
        extended = self.tx_format.get() == "Extended"
        try:
            can_id = int(self.tx_id.get().strip().removeprefix("0x"), 16)
            maximum = 0x1FFFFFFF if extended else 0x7FF
            if not 0 <= can_id <= maximum:
                raise ValueError("CAN ID is outside the selected format")
            tokens = self.tx_data.get().replace(",", " ").split()
            data = bytes(int(token, 16) for token in tokens)
            if len(data) > 8 or any(len(token) > 2 for token in tokens):
                raise ValueError("Enter at most eight hexadecimal bytes")
        except ValueError as exc:
            messagebox.showerror("Invalid CAN frame", str(exc))
            return

        remote = self.tx_remote.get()
        flags = (0x01 if extended else 0) | (0x02 if remote else 0)
        packet = struct.pack("<BBBI", 0x01, flags, 0 if remote else len(data), can_id)
        if not remote:
            packet += data
        self.submit_write(packet, f"Queued 0x{can_id:X}")

    def set_bitrate(self, _event: object | None = None) -> None:
        values = {
            "125 kbit/s": 125000,
            "250 kbit/s": 250000,
            "500 kbit/s": 500000,
            "1 Mbit/s": 1000000,
        }
        bitrate = values[self.bitrate.get()]
        self.submit_write(struct.pack("<BI", 0x02, bitrate), self.bitrate.get())

    def submit_write(self, packet: bytes, success: str) -> None:
        if (
            not self.loop
            or not self.client
            or not self.client.is_connected
            or not self.command_characteristic
        ):
            self.tx_status.set("Connect to BLE first")
            return
        future = asyncio.run_coroutine_threadsafe(
            self.client.write_gatt_char(
                self.command_characteristic, packet, response=True
            ),
            self.loop,
        )

        def completed(result: object) -> None:
            try:
                future.result()
                self.messages.put({"_tx_status": success})
            except Exception as exc:
                self.messages.put({"_tx_status": f"Failed: {exc}"})

        future.add_done_callback(completed)

    def remote_changed(self) -> None:
        self.data_entry.configure(
            state=tk.DISABLED if self.tx_remote.get() else tk.NORMAL
        )

    def drain(self) -> None:
        try:
            while True:
                frame = self.messages.get_nowait()
                if "_status" in frame:
                    self.status.set(str(frame["_status"]))
                    continue
                if "_tx_status" in frame:
                    self.tx_status.set(str(frame["_tx_status"]))
                    continue
                self.frames.append(frame)
                data = " ".join(f"{int(value):02X}" for value in frame["data"])
                kind = "EXT" if frame["extended"] else "STD"
                if frame["remote"]:
                    kind += " RTR"
                values = (
                    f"{int(frame['timestamp_us']) / 1_000_000:.3f}",
                    str(frame["direction"]).upper(),
                    frame["id_hex"],
                    kind,
                    frame["dlc"],
                    data,
                )
                self.table.insert("", 0, values=values)
                if len(self.table.get_children()) > 2000:
                    self.table.delete(self.table.get_children()[-1])
        except queue.Empty:
            pass
        self.root.after(50, self.drain)

    def export(self) -> None:
        name = filedialog.asksaveasfilename(
            defaultextension=".csv", filetypes=[("CSV files", "*.csv")]
        )
        if not name:
            return
        with Path(name).open("w", newline="", encoding="utf-8") as output:
            writer = csv.writer(output)
            writer.writerow(
                ("timestamp_us", "direction", "id", "extended", "remote", "dlc", "data")
            )
            for frame in self.frames:
                writer.writerow(
                    (
                        frame["timestamp_us"],
                        frame["direction"],
                        frame["id_hex"],
                        frame["extended"],
                        frame["remote"],
                        frame["dlc"],
                        " ".join(f"{int(value):02X}" for value in frame["data"]),
                    )
                )

    async def disconnect(self) -> None:
        if self.client and self.client.is_connected:
            await self.client.disconnect()

    def close(self) -> None:
        self.stop.set()
        if self.loop and self.loop.is_running():
            asyncio.run_coroutine_threadsafe(self.disconnect(), self.loop)
        self.root.destroy()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--device", default="ESP32-CAN", help="BLE advertising name (default: ESP32-CAN)"
    )
    args = parser.parse_args()
    root = tk.Tk()
    GatewayMonitor(root, args.device)
    root.mainloop()


if __name__ == "__main__":
    main()

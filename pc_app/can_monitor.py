#!/usr/bin/env python3
"""Small desktop monitor for the ESP32 CAN gateway WebSocket stream."""

from __future__ import annotations

import argparse
import csv
import json
import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk

import websocket


class GatewayMonitor:
    def __init__(self, root: tk.Tk, host: str) -> None:
        self.root = root
        self.root.title("ESP32 CAN Gateway")
        self.root.geometry("900x560")
        self.messages: queue.Queue[dict[str, object]] = queue.Queue()
        self.frames: list[dict[str, object]] = []
        self.stop = threading.Event()

        bar = ttk.Frame(root, padding=10)
        bar.pack(fill=tk.X)
        ttk.Label(bar, text="Gateway:").pack(side=tk.LEFT)
        self.host = tk.StringVar(value=host)
        ttk.Entry(bar, textvariable=self.host, width=32).pack(side=tk.LEFT, padx=6)
        ttk.Button(bar, text="Connect", command=self.connect).pack(side=tk.LEFT)
        ttk.Button(bar, text="Export CSV", command=self.export).pack(side=tk.RIGHT)
        self.status = tk.StringVar(value="Disconnected")
        ttk.Label(root, textvariable=self.status, padding=(10, 0)).pack(anchor=tk.W)

        columns = ("time", "id", "type", "dlc", "data")
        self.table = ttk.Treeview(root, columns=columns, show="headings")
        for name, width in zip(columns, (130, 120, 100, 60, 380)):
            self.table.heading(name, text=name.upper())
            self.table.column(name, width=width, anchor=tk.W)
        self.table.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        self.root.after(50, self.drain)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def connect(self) -> None:
        self.stop.set()
        self.stop = threading.Event()
        host = self.host.get().strip().removeprefix("http://").removeprefix("https://").rstrip("/")
        threading.Thread(target=self.receive, args=(host,), daemon=True).start()

    def receive(self, host: str) -> None:
        self.messages.put({"_status": f"Connecting to {host}…"})
        while not self.stop.is_set():
            try:
                ws = websocket.create_connection(f"ws://{host}/ws", timeout=5)
                self.messages.put({"_status": "Connected"})
                while not self.stop.is_set():
                    self.messages.put(json.loads(ws.recv()))
            except Exception as exc:  # Connection errors are displayed and retried.
                self.messages.put({"_status": f"Disconnected: {exc}"})
                self.stop.wait(2)

    def drain(self) -> None:
        try:
            while True:
                frame = self.messages.get_nowait()
                if "_status" in frame:
                    self.status.set(str(frame["_status"]))
                    continue
                self.frames.append(frame)
                data = " ".join(f"{int(value):02X}" for value in frame.get("data", []))
                kind = "EXT" if frame.get("extended") else "STD"
                if frame.get("remote"):
                    kind += " RTR"
                values = (
                    f"{int(frame['timestamp_us']) / 1_000_000:.3f}",
                    frame.get("id_hex", ""),
                    kind,
                    frame.get("dlc", 0),
                    data,
                )
                self.table.insert("", 0, values=values)
                if len(self.table.get_children()) > 1000:
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
            writer.writerow(("timestamp_us", "id", "extended", "remote", "dlc", "data"))
            for frame in self.frames:
                writer.writerow(
                    (
                        frame.get("timestamp_us"),
                        frame.get("id_hex"),
                        frame.get("extended"),
                        frame.get("remote"),
                        frame.get("dlc"),
                        " ".join(f"{int(value):02X}" for value in frame.get("data", [])),
                    )
                )

    def close(self) -> None:
        self.stop.set()
        self.root.destroy()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="esp32-can.local", help="ESP32 IP address or hostname")
    args = parser.parse_args()
    root = tk.Tk()
    GatewayMonitor(root, args.host)
    root.mainloop()


if __name__ == "__main__":
    main()

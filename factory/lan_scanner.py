#!/usr/bin/env python3
"""Desktop LAN scanner for SMS Forwarder devices."""

from __future__ import annotations

import argparse
import json
import queue
import sys
import threading
import webbrowser
from typing import Iterable

try:
    from .lan_discovery import DiscoveredDevice, discover_devices, local_ipv4_addresses
except ImportError:
    from lan_discovery import DiscoveredDevice, discover_devices, local_ipv4_addresses


APP_NAME = "SMS Forwarder 局域网发现工具"


class ScannerApp:
    def __init__(self, root) -> None:
        import tkinter as tk
        from tkinter import messagebox, ttk

        self.root = root
        self.tk = tk
        self.ttk = ttk
        self.messagebox = messagebox
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.devices: dict[str, DiscoveredDevice] = {}
        self.rows: dict[str, str] = {}
        self.scanning = False
        self.cancel_event = threading.Event()

        root.title(APP_NAME)
        root.geometry("1080x610")
        root.minsize(820, 440)
        try:
            root.tk.call("tk", "scaling", max(1.0, root.winfo_fpixels("1i") / 96.0))
        except Exception:
            pass

        style = ttk.Style(root)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Title.TLabel", font=("Microsoft YaHei UI", 19, "bold"))
        style.configure("Hint.TLabel", foreground="#5f6470")

        frame = ttk.Frame(root, padding=18)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="SMS Forwarder 设备发现", style="Title.TLabel").pack(anchor="w")
        ttk.Label(
            frame,
            text="自动查找同一局域网内的设备。发现报文不会读取手机号、ICCID、Wi-Fi 密码或通知密钥。",
            style="Hint.TLabel",
        ).pack(anchor="w", pady=(3, 14))

        toolbar = ttk.Frame(frame)
        toolbar.pack(fill="x", pady=(0, 10))
        self.scan_button = ttk.Button(toolbar, text="扫描局域网", command=self.start_scan)
        self.scan_button.pack(side="left")
        self.open_button = ttk.Button(toolbar, text="打开管理页面", command=self.open_selected, state="disabled")
        self.open_button.pack(side="left", padx=8)
        self.copy_button = ttk.Button(toolbar, text="复制 IP", command=self.copy_selected, state="disabled")
        self.copy_button.pack(side="left")
        self.status_var = tk.StringVar(value="准备扫描")
        ttk.Label(toolbar, textvariable=self.status_var, style="Hint.TLabel").pack(side="right")

        columns = ("ip", "firmware", "modem", "ota", "mode", "mac", "chip")
        self.tree = ttk.Treeview(frame, columns=columns, show="headings", selectmode="browse", height=15)
        settings = (
            ("ip", "IP 地址", 135),
            ("firmware", "固件版本", 150),
            ("modem", "蜂窝模组", 105),
            ("ota", "在线 OTA", 90),
            ("mode", "网络模式", 100),
            ("mac", "Wi-Fi MAC", 155),
            ("chip", "设备 ID", 140),
        )
        for key, title, width in settings:
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, minwidth=70, anchor="w")
        scroll = ttk.Scrollbar(frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", self.selection_changed)
        self.tree.bind("<Double-1>", lambda _event: self.open_selected())

        footer = ttk.Frame(root, padding=(18, 0, 18, 14))
        footer.pack(fill="x")
        self.adapters_var = tk.StringVar()
        ttk.Label(footer, textvariable=self.adapters_var, style="Hint.TLabel").pack(anchor="w")
        self.update_adapters()
        root.after(100, self.poll_events)
        root.after(350, self.start_scan)

    def update_adapters(self) -> None:
        addresses = local_ipv4_addresses()
        self.adapters_var.set("本机 IPv4：" + ("、".join(addresses) if addresses else "未检测到"))

    def start_scan(self) -> None:
        if self.scanning:
            return
        self.scanning = True
        self.cancel_event.clear()
        self.scan_button.configure(state="disabled", text="正在扫描…")
        self.status_var.set("正在广播发现请求，约需 3 秒")
        self.update_adapters()

        def worker() -> None:
            try:
                result = discover_devices(
                    timeout=3.5,
                    on_device=lambda device: self.events.put(("device", device)),
                    cancelled=self.cancel_event.is_set,
                )
                self.events.put(("done", result))
            except Exception as exc:
                self.events.put(("error", str(exc)))

        threading.Thread(target=worker, daemon=True).start()

    def add_device(self, device: DiscoveredDevice) -> None:
        self.devices[device.identity] = device
        values = (
            device.ip,
            device.firmware,
            device.modem,
            "支持" if device.ota else "不支持",
            "配置热点" if device.ap_mode else "局域网",
            device.mac,
            device.chip_id,
        )
        if device.identity in self.rows:
            self.tree.item(self.rows[device.identity], values=values)
        else:
            row = self.tree.insert("", "end", values=values)
            self.rows[device.identity] = row
            if len(self.rows) == 1:
                self.tree.selection_set(row)
        self.status_var.set(f"已发现 {len(self.devices)} 台设备，继续扫描…")

    def selected_device(self) -> DiscoveredDevice | None:
        selection = self.tree.selection()
        if not selection:
            return None
        row = selection[0]
        for identity, item in self.rows.items():
            if item == row:
                return self.devices.get(identity)
        return None

    def selection_changed(self, _event=None) -> None:
        state = "normal" if self.selected_device() else "disabled"
        self.open_button.configure(state=state)
        self.copy_button.configure(state=state)

    def open_selected(self) -> None:
        device = self.selected_device()
        if device:
            webbrowser.open(device.url)

    def copy_selected(self) -> None:
        device = self.selected_device()
        if not device:
            return
        self.root.clipboard_clear()
        self.root.clipboard_append(device.ip)
        self.status_var.set(f"已复制 {device.ip}")

    def poll_events(self) -> None:
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "device":
                    self.add_device(value)
                elif kind == "done":
                    self.scanning = False
                    self.scan_button.configure(state="normal", text="重新扫描")
                    count = len(value)
                    self.status_var.set(
                        f"扫描完成，共发现 {count} 台设备"
                        if count
                        else "未发现设备；请确认电脑与设备在同一局域网且未启用 AP 隔离"
                    )
                elif kind == "error":
                    self.scanning = False
                    self.scan_button.configure(state="normal", text="重新扫描")
                    self.status_var.set("扫描失败")
                    self.messagebox.showerror(APP_NAME, str(value))
        except queue.Empty:
            pass
        self.root.after(100, self.poll_events)


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=APP_NAME)
    parser.add_argument("--json", action="store_true", help="扫描后输出 JSON，不打开窗口")
    parser.add_argument("--timeout", type=float, default=3.5, help="扫描秒数，范围 0.5-30")
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(argv)
    if args.json:
        devices = discover_devices(args.timeout)
        print(json.dumps([item.to_dict() for item in devices], ensure_ascii=False, indent=2))
        return 0
    import tkinter as tk

    root = tk.Tk()
    ScannerApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

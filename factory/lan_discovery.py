#!/usr/bin/env python3
"""UDP discovery protocol used by the SMS Forwarder LAN scanner."""

from __future__ import annotations

import hmac
import ipaddress
import json
import re
import secrets
import select
import socket
import time
from dataclasses import asdict, dataclass
from typing import Callable


DISCOVERY_PORT = 37888
REQUEST_PREFIX = "SMS_FORWARDER_DISCOVER_V1 "
PRODUCT = "sms-forwarder-board"
MAX_RESPONSE_SIZE = 2048
MAC_PATTERN = re.compile(r"^(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")
CHIP_ID_PATTERN = re.compile(r"^[0-9A-Fa-f]{12}$")


@dataclass(frozen=True)
class DiscoveredDevice:
    ip: str
    port: int
    mac: str
    chip_id: str
    firmware: str
    modem: str
    ota: bool
    ap_mode: bool

    @property
    def identity(self) -> str:
        return self.chip_id or self.mac or self.ip

    @property
    def url(self) -> str:
        suffix = "" if self.port == 80 else f":{self.port}"
        return f"http://{self.ip}{suffix}/"

    def to_dict(self) -> dict[str, object]:
        result = asdict(self)
        result["url"] = self.url
        return result


def _short_text(value: object, limit: int = 64) -> str:
    text = str(value or "").strip()
    if not text or len(text) > limit or any(ord(char) < 32 for char in text):
        raise ValueError("invalid discovery text field")
    return text


def parse_response(data: bytes, source_ip: str, expected_nonce: str) -> DiscoveredDevice:
    if not data or len(data) > MAX_RESPONSE_SIZE:
        raise ValueError("invalid discovery response size")
    if not re.fullmatch(r"[0-9a-f]{16}", expected_nonce):
        raise ValueError("invalid discovery nonce")
    payload = json.loads(data.decode("utf-8", errors="strict"))
    if not isinstance(payload, dict):
        raise ValueError("discovery response is not an object")
    if payload.get("protocol") != 1 or payload.get("product") != PRODUCT:
        raise ValueError("incompatible discovery response")
    nonce = str(payload.get("nonce", ""))
    if not hmac.compare_digest(nonce, expected_nonce):
        raise ValueError("discovery nonce mismatch")

    source = ipaddress.ip_address(source_ip)
    reported = ipaddress.ip_address(str(payload.get("ip", "")))
    if source.version != 4 or reported != source or not reported.is_private:
        raise ValueError("discovery source address mismatch")
    port = payload.get("port")
    if not isinstance(port, int) or isinstance(port, bool) or not 1 <= port <= 65535:
        raise ValueError("invalid management port")
    mac = _short_text(payload.get("mac"), 17).upper()
    chip_id = _short_text(payload.get("chipId"), 12).upper()
    if not MAC_PATTERN.fullmatch(mac) or not CHIP_ID_PATTERN.fullmatch(chip_id):
        raise ValueError("invalid device identity")
    if not isinstance(payload.get("ota"), bool) or not isinstance(payload.get("apMode"), bool):
        raise ValueError("invalid device capability flags")
    return DiscoveredDevice(
        ip=str(reported),
        port=port,
        mac=mac,
        chip_id=chip_id,
        firmware=_short_text(payload.get("firmware")),
        modem=_short_text(payload.get("modem")),
        ota=payload["ota"],
        ap_mode=payload["apMode"],
    )


def local_ipv4_addresses() -> list[str]:
    addresses: set[str] = set()
    try:
        for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET, socket.SOCK_DGRAM):
            addresses.add(item[4][0])
    except OSError:
        pass
    # A UDP connect selects the system's preferred route without sending data.
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("1.1.1.1", 53))
        addresses.add(probe.getsockname()[0])
    except OSError:
        pass
    finally:
        probe.close()
    result: list[str] = []
    for value in addresses:
        try:
            address = ipaddress.ip_address(value)
        except ValueError:
            continue
        if address.version == 4 and not address.is_loopback and not address.is_link_local:
            result.append(str(address))
    return sorted(result, key=lambda value: tuple(int(part) for part in value.split(".")))


def broadcast_targets(local_ip: str) -> tuple[str, ...]:
    address = ipaddress.ip_address(local_ip)
    if address.version != 4 or address.is_loopback:
        raise ValueError("invalid local IPv4 address")
    # Limited broadcast covers the actual adapter mask. The /24 target also
    # handles Windows networks where limited broadcasts are filtered.
    octets = local_ip.split(".")
    return "255.255.255.255", ".".join((*octets[:3], "255"))


def _open_sockets() -> list[tuple[socket.socket, str]]:
    result: list[tuple[socket.socket, str]] = []
    addresses = local_ipv4_addresses()
    for address in addresses or ["0.0.0.0"]:
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            udp.bind((address, 0))
            udp.setblocking(False)
            result.append((udp, address))
        except OSError:
            udp.close()
    if not result:
        raise RuntimeError("没有可用于扫描的 IPv4 网络接口")
    return result


def discover_devices(
    timeout: float = 3.2,
    on_device: Callable[[DiscoveredDevice], None] | None = None,
    cancelled: Callable[[], bool] | None = None,
) -> list[DiscoveredDevice]:
    if not 0.5 <= timeout <= 30:
        raise ValueError("scan timeout must be between 0.5 and 30 seconds")
    nonce = secrets.token_hex(8)
    request = (REQUEST_PREFIX + nonce).encode("ascii")
    sockets = _open_sockets()
    devices: dict[str, DiscoveredDevice] = {}
    deadline = time.monotonic() + timeout
    next_send = 0.0
    try:
        while time.monotonic() < deadline:
            if cancelled and cancelled():
                break
            now = time.monotonic()
            if now >= next_send:
                for udp, local_ip in sockets:
                    targets = {"255.255.255.255"}
                    if local_ip != "0.0.0.0":
                        targets.update(broadcast_targets(local_ip))
                    for target in targets:
                        try:
                            udp.sendto(request, (target, DISCOVERY_PORT))
                        except OSError:
                            pass
                next_send = now + 0.75
            readable, _, _ = select.select([item[0] for item in sockets], [], [], 0.12)
            for udp in readable:
                try:
                    data, source = udp.recvfrom(MAX_RESPONSE_SIZE + 1)
                    device = parse_response(data, source[0], nonce)
                except (OSError, UnicodeError, ValueError, json.JSONDecodeError):
                    continue
                previous = devices.get(device.identity)
                devices[device.identity] = device
                if on_device and device != previous:
                    on_device(device)
    finally:
        for udp, _ in sockets:
            udp.close()
    return sorted(devices.values(), key=lambda item: tuple(int(part) for part in item.ip.split(".")))

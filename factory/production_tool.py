#!/usr/bin/env python3
"""Windows batch flasher and factory tester for the SMS forwarding board."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable


APP_NAME = "SMS 转发器量产工具"
FACTORY_PREFIX = b"@@FACTORY:"
SUPPORTED_MODEMS = {"ML307A", "ML307C", "ML307R", "ML307Y"}
DEFAULT_GITHUB_REPOSITORY = "sukiyra/sms-forwarder-board"
LOCAL_FIRMWARE_LABEL = "本地固件目录"
MAX_RELEASE_DOWNLOAD = 128 * 1024 * 1024
MAX_RELEASE_UNPACKED = 192 * 1024 * 1024


def application_root() -> Path:
    return Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent


def firmware_cache_root() -> Path:
    base = os.environ.get("LOCALAPPDATA")
    if base:
        return Path(base) / "SMS Forwarder Production Tool" / "firmware"
    return Path.home() / ".cache" / "sms-forwarder-production-tool" / "firmware"


def import_serial():
    try:
        import serial  # type: ignore
        from serial.tools import list_ports  # type: ignore
    except ImportError as exc:
        raise RuntimeError("缺少 pyserial，请运行 factory/start.ps1 自动安装") from exc
    return serial, list_ports


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclass(frozen=True)
class FirmwareImage:
    offset: str
    path: Path


@dataclass(frozen=True)
class FirmwareBundle:
    root: Path
    version: str
    images: tuple[FirmwareImage, ...]

    @classmethod
    def load(cls, root: Path) -> "FirmwareBundle":
        root = root.resolve()
        manifest_path = root / "manifest.json"
        if not manifest_path.is_file():
            raise RuntimeError(f"找不到固件清单：{manifest_path}")
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        if data.get("chip") != "esp32c3":
            raise RuntimeError("固件清单不是 ESP32-C3")
        images: list[FirmwareImage] = []
        for item in data.get("images", []):
            relative = Path(str(item["file"]).replace("\\", "/"))
            if relative.is_absolute() or ".." in relative.parts or not relative.parts:
                raise RuntimeError("固件清单包含不安全路径")
            path = (root / relative).resolve()
            if root not in path.parents:
                raise RuntimeError("固件清单包含越界路径")
            if not path.is_file():
                raise RuntimeError(f"固件文件缺失：{path.name}")
            expected = str(item.get("sha256", "")).lower()
            actual = sha256_file(path)
            if expected and actual != expected:
                raise RuntimeError(f"固件校验失败：{path.name}")
            images.append(FirmwareImage(str(item["offset"]), path))
        if not images:
            raise RuntimeError("固件清单没有可烧录镜像")
        return cls(root, str(data.get("version", "unknown")), tuple(images))


@dataclass(frozen=True)
class ReleaseVersion:
    tag: str
    name: str
    asset_name: str
    asset_url: str
    published_at: str
    prerelease: bool = False

    @property
    def label(self) -> str:
        suffix = "（预发布）" if self.prerelease else ""
        return f"{self.tag}{suffix}"


def parse_release_versions(payload: Any) -> list[ReleaseVersion]:
    if not isinstance(payload, list):
        raise RuntimeError("GitHub 版本接口返回格式无效")
    versions: list[ReleaseVersion] = []
    for release in payload:
        if not isinstance(release, dict) or release.get("draft"):
            continue
        assets = release.get("assets") or []
        candidates = [
            asset for asset in assets
            if isinstance(asset, dict)
            and str(asset.get("name", "")).lower().endswith(".zip")
            and "firmware" in str(asset.get("name", "")).lower()
            and str(asset.get("browser_download_url", "")).startswith("https://github.com/")
        ]
        if not candidates:
            continue
        asset = candidates[0]
        tag = str(release.get("tag_name", "")).strip()
        if not tag:
            continue
        versions.append(
            ReleaseVersion(
                tag=tag,
                name=str(release.get("name") or tag),
                asset_name=str(asset["name"]),
                asset_url=str(asset["browser_download_url"]),
                published_at=str(release.get("published_at") or ""),
                prerelease=bool(release.get("prerelease")),
            )
        )
    return versions


def normalized_version(value: str) -> str:
    result = value.strip().lower()
    if result.startswith("sukiyra-"):
        result = result[len("sukiyra-") :]
    if result.startswith("v"):
        result = result[1:]
    return result


def safe_release_name(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip(".-")
    if not result:
        raise RuntimeError("GitHub 版本号无效")
    return result


def install_release_archive(archive: Path, cache_root: Path, release: ReleaseVersion) -> FirmwareBundle:
    cache_root.mkdir(parents=True, exist_ok=True)
    destination = cache_root / safe_release_name(release.tag)
    with tempfile.TemporaryDirectory(prefix="unpack-", dir=cache_root) as temp_folder:
        unpacked = Path(temp_folder)
        total_size = 0
        with zipfile.ZipFile(archive) as package:
            members = package.infolist()
            if len(members) > 128:
                raise RuntimeError("固件压缩包文件数量异常")
            for member in members:
                parts = Path(member.filename.replace("\\", "/")).parts
                if not parts or Path(member.filename).is_absolute() or ".." in parts:
                    raise RuntimeError("固件压缩包包含不安全路径")
                file_type = (member.external_attr >> 16) & 0o170000
                if file_type == 0o120000:
                    raise RuntimeError("固件压缩包不能包含符号链接")
                total_size += member.file_size
                if total_size > MAX_RELEASE_UNPACKED:
                    raise RuntimeError("固件压缩包解压后体积异常")
                target = (unpacked.joinpath(*parts)).resolve()
                if unpacked.resolve() not in target.parents and target != unpacked.resolve():
                    raise RuntimeError("固件压缩包包含越界路径")
            package.extractall(unpacked)
        manifests = list(unpacked.rglob("manifest.json"))
        if len(manifests) != 1:
            raise RuntimeError("固件压缩包必须包含唯一的 manifest.json")
        bundle = FirmwareBundle.load(manifests[0].parent)
        if normalized_version(bundle.version) != normalized_version(release.tag):
            raise RuntimeError(f"版本不匹配：选择 {release.tag}，压缩包为 {bundle.version}")
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(bundle.root, destination)
    return FirmwareBundle.load(destination)


class GitHubReleaseStore:
    def __init__(self, repository: str = DEFAULT_GITHUB_REPOSITORY, cache_root: Path | None = None) -> None:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise RuntimeError("GitHub 仓库格式应为 owner/repository")
        self.repository = repository
        self.cache_root = (cache_root or firmware_cache_root()).resolve()

    def _request(self, url: str, *, api: bool = True) -> urllib.request.Request:
        headers = {
            "Accept": "application/vnd.github+json" if api else "application/octet-stream",
            "User-Agent": "sms-forwarder-production-tool",
            "X-GitHub-Api-Version": "2022-11-28",
        }
        token = os.environ.get("GITHUB_TOKEN", "").strip()
        if token:
            headers["Authorization"] = f"Bearer {token}"
        return urllib.request.Request(url, headers=headers)

    def list_versions(self) -> list[ReleaseVersion]:
        url = f"https://api.github.com/repos/{self.repository}/releases?per_page=100"
        try:
            with urllib.request.urlopen(self._request(url), timeout=20) as response:
                payload = json.load(response)
        except urllib.error.HTTPError as exc:
            if exc.code == 403:
                raise RuntimeError("GitHub 访问频率受限，请稍后重试") from exc
            raise RuntimeError(f"GitHub 版本读取失败（HTTP {exc.code}）") from exc
        except (OSError, ValueError) as exc:
            raise RuntimeError(f"无法连接 GitHub：{exc}") from exc
        versions = parse_release_versions(payload)
        if not versions:
            raise RuntimeError("GitHub Releases 中没有可用的固件 ZIP")
        return versions

    def resolve(self, tag: str, log: Callable[[str], None] = print) -> FirmwareBundle:
        versions = self.list_versions()
        release = versions[0] if tag.lower() == "latest" else next(
            (item for item in versions if item.tag == tag or item.label == tag), None
        )
        if release is None:
            raise RuntimeError(f"GitHub 中没有版本：{tag}")
        cached = self.cache_root / safe_release_name(release.tag)
        if (cached / "manifest.json").is_file():
            try:
                bundle = FirmwareBundle.load(cached)
                if normalized_version(bundle.version) == normalized_version(release.tag):
                    log(f"使用已缓存固件 {release.tag}")
                    return bundle
            except Exception:
                pass
        self.cache_root.mkdir(parents=True, exist_ok=True)
        download = self.cache_root / f".{safe_release_name(release.tag)}.download.zip"
        log(f"正在从 GitHub 下载固件 {release.tag}")
        try:
            request = self._request(release.asset_url, api=False)
            with urllib.request.urlopen(request, timeout=60) as response, download.open("wb") as target:
                declared = int(response.headers.get("Content-Length") or 0)
                if declared > MAX_RELEASE_DOWNLOAD:
                    raise RuntimeError("GitHub 固件压缩包体积异常")
                received = 0
                while True:
                    block = response.read(1024 * 1024)
                    if not block:
                        break
                    received += len(block)
                    if received > MAX_RELEASE_DOWNLOAD:
                        raise RuntimeError("GitHub 固件压缩包超过大小限制")
                    target.write(block)
            bundle = install_release_archive(download, self.cache_root, release)
            log(f"固件 {release.tag} 下载并校验完成")
            return bundle
        except urllib.error.HTTPError as exc:
            raise RuntimeError(f"GitHub 固件下载失败（HTTP {exc.code}）") from exc
        except (OSError, zipfile.BadZipFile) as exc:
            raise RuntimeError(f"GitHub 固件下载或解压失败：{exc}") from exc
        finally:
            download.unlink(missing_ok=True)


@dataclass(frozen=True)
class SerialPortInfo:
    device: str
    description: str
    hwid: str
    likely_esp32: bool


def scan_ports() -> list[SerialPortInfo]:
    _, list_ports = import_serial()
    result: list[SerialPortInfo] = []
    for item in list_ports.comports():
        text = f"{item.description} {item.hwid}".upper()
        likely = item.vid == 0x303A or any(
            marker in text for marker in ("ESP32", "ESPRESSIF", "CP210", "CH340", "CH910")
        )
        if "BLUETOOTH" in text:
            continue
        result.append(SerialPortInfo(item.device, item.description or "串口", item.hwid or "", likely))
    return sorted(result, key=lambda value: value.device)


def find_esptool(explicit: str | None = None) -> Path:
    if explicit:
        candidate = Path(explicit).expanduser()
        if candidate.is_file():
            return candidate.resolve()
        raise RuntimeError(f"找不到 esptool：{candidate}")
    bundled = application_root() / "esptool.exe"
    if bundled.is_file():
        return bundled.resolve()
    command = shutil.which("esptool") or shutil.which("esptool.exe")
    if command:
        return Path(command).resolve()
    arduino_root = Path(os.environ.get("LOCALAPPDATA", "")) / "Arduino15" / "packages" / "esp32" / "tools" / "esptool_py"
    candidates = sorted(arduino_root.glob("*/esptool.exe"), reverse=True)
    if candidates:
        return candidates[0].resolve()
    raise RuntimeError("找不到 esptool。请先运行 factory/build_firmware.ps1 安装 ESP32 工具链")


def run_process(command: list[str], log: Callable[[str], None], timeout: int = 180) -> str:
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    lines: list[str] = []
    assert process.stdout is not None
    started = time.monotonic()
    while True:
        line = process.stdout.readline()
        if line:
            value = line.rstrip()
            lines.append(value)
            if value:
                log(value)
        if process.poll() is not None:
            break
        if time.monotonic() - started > timeout:
            process.kill()
            raise RuntimeError("命令执行超时")
    if process.returncode:
        detail = "\n".join(lines[-12:])
        raise RuntimeError(f"命令失败（退出码 {process.returncode}）\n{detail}")
    return "\n".join(lines)


def flash_device(
    port: str,
    bundle: FirmwareBundle,
    esptool: Path,
    erase: bool,
    log: Callable[[str], None],
) -> None:
    base = [str(esptool), "--chip", "esp32c3", "--port", port, "--baud", "921600"]
    if erase:
        run_process(base + ["erase-flash"], log)
    write = base + [
        "--after",
        "hard-reset",
        "write-flash",
        "--flash-mode",
        "dio",
        "--flash-freq",
        "80m",
        "--flash-size",
        "4MB",
    ]
    for image in bundle.images:
        write.extend((image.offset, str(image.path)))
    try:
        run_process(write, log, timeout=240)
    except RuntimeError:
        # Long USB hubs and marginal cables are often stable at 460800.
        log("921600 烧录失败，自动降速到 460800 重试")
        write[write.index("921600")] = "460800"
        run_process(write, log, timeout=300)


def factory_status(
    port: str,
    timeout: int,
    log: Callable[[str], None],
    accept: Callable[[dict[str, Any]], bool] | None = None,
) -> dict[str, Any]:
    serial, _ = import_serial()
    deadline = time.monotonic() + timeout
    last_open_error = ""
    last_status: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        try:
            with serial.Serial(port, 115200, timeout=0.25, write_timeout=1) as link:
                link.reset_input_buffer()
                while time.monotonic() < deadline:
                    link.write(b"FACTORY STATUS\r\n")
                    link.flush()
                    read_until = min(deadline, time.monotonic() + 2.5)
                    while time.monotonic() < read_until:
                        line = link.readline().strip()
                        if not line.startswith(FACTORY_PREFIX):
                            continue
                        payload = line[len(FACTORY_PREFIX) :].decode("utf-8", "replace")
                        status = json.loads(payload)
                        last_status = status
                        if accept is None or accept(status):
                            log("已读取固件工厂状态")
                            return status
                    time.sleep(0.5)
        except (OSError, serial.SerialException) as exc:
            last_open_error = str(exc)
            time.sleep(1)
    if last_status is not None:
        log("自检等待到期，使用最后一次工厂状态判定")
        return last_status
    suffix = f"：{last_open_error}" if last_open_error else ""
    raise RuntimeError(f"等待固件工厂状态超时{suffix}")


def evaluate_status(status: dict[str, Any], require_sim: bool, require_network: bool) -> list[str]:
    failures: list[str] = []
    chip = status.get("chip") or {}
    modem = status.get("modem") or {}
    sim = status.get("sim") or {}
    if str(chip.get("model", "")).upper() != "ESP32-C3":
        failures.append("主控不是 ESP32-C3")
    family = str(modem.get("family", "")).upper()
    if not modem.get("supported") or family not in SUPPORTED_MODEMS:
        failures.append(f"模组未识别或不受支持：{family or 'unknown'}")
    if require_sim and not sim.get("ready"):
        failures.append("SIM 未就绪")
    if require_sim and (not sim.get("smsReady") or modem.get("smsMode") not in {"direct", "stored"}):
        failures.append("短信接口未配置")
    if require_network and not modem.get("registered"):
        failures.append("未完成蜂窝网络注册")
    return failures


_record_lock = threading.Lock()


def save_record(record_root: Path, port: str, status: dict[str, Any], result: str, error: str) -> None:
    record_root.mkdir(parents=True, exist_ok=True)
    chip = status.get("chip") or {}
    modem = status.get("modem") or {}
    sim = status.get("sim") or {}
    network = status.get("network") or {}
    now = datetime.now(timezone.utc).astimezone()
    row = {
        "time": now.isoformat(timespec="seconds"),
        "result": result,
        "port": port,
        "chip_id": chip.get("id", ""),
        "firmware": status.get("firmware", ""),
        "modem_family": modem.get("family", ""),
        "modem_model": modem.get("model", ""),
        "modem_firmware": modem.get("firmware", ""),
        "sim_type": sim.get("type", ""),
        "iccid_tail": sim.get("iccidTail", ""),
        "home_plmn": sim.get("homePlmn", ""),
        "network_plmn": network.get("plmn", ""),
        "error": error,
    }
    with _record_lock:
        csv_path = record_root / "production.csv"
        new_file = not csv_path.exists()
        with csv_path.open("a", newline="", encoding="utf-8-sig") as target:
            writer = csv.DictWriter(target, fieldnames=list(row))
            if new_file:
                writer.writeheader()
            writer.writerow(row)
        safe_id = "".join(c for c in str(chip.get("id") or port) if c.isalnum() or c in "-_")
        detail = {"result": result, "error": error, "port": port, "capturedAt": row["time"], "status": status}
        (record_root / f"{now:%Y%m%d-%H%M%S}-{safe_id}.json").write_text(
            json.dumps(detail, ensure_ascii=False, indent=2), encoding="utf-8"
        )


def produce_one(
    port: str,
    bundle: FirmwareBundle | None,
    esptool: Path,
    record_root: Path,
    erase: bool,
    skip_flash: bool,
    require_sim: bool,
    require_network: bool,
    timeout: int,
    log: Callable[[str], None],
) -> tuple[bool, dict[str, Any], str]:
    status: dict[str, Any] = {}
    error = ""
    try:
        if not skip_flash:
            if bundle is None:
                raise RuntimeError("未提供固件包")
            log(f"[{port}] 开始烧录 {bundle.version}")
            flash_device(port, bundle, esptool, erase, lambda line: log(f"[{port}] {line}"))
        log(f"[{port}] 等待设备和模组自检")
        status = factory_status(
            port,
            timeout,
            lambda line: log(f"[{port}] {line}"),
            lambda value: not evaluate_status(value, require_sim, require_network),
        )
        failures = evaluate_status(status, require_sim, require_network)
        if failures:
            error = "；".join(failures)
            raise RuntimeError(error)
        log(f"[{port}] PASS · {status.get('firmware')} · {(status.get('modem') or {}).get('model')}")
        save_record(record_root, port, status, "PASS", "")
        return True, status, ""
    except Exception as exc:  # one failed unit must not stop the batch
        error = str(exc)
        log(f"[{port}] FAIL · {error}")
        try:
            save_record(record_root, port, status, "FAIL", error)
        except Exception as record_exc:
            error += f"；生产记录写入失败：{record_exc}"
            log(f"[{port}] {error}")
        return False, status, error


class ProductionApp:
    def __init__(self, root: Any, defaults: argparse.Namespace) -> None:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk

        self.tk = tk
        self.ttk = ttk
        self.filedialog = filedialog
        self.messagebox = messagebox
        self.root = root
        self.root.title(APP_NAME)
        self.root.geometry("980x740")
        self.events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.running = False
        self.versions_loading = False
        self.port_rows: dict[str, str] = {}
        self.release_rows: dict[str, ReleaseVersion] = {}
        self.release_store = GitHubReleaseStore(defaults.github_repo, defaults.firmware_cache)

        frame = ttk.Frame(root, padding=14)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text=APP_NAME, font=("Microsoft YaHei UI", 18, "bold")).pack(anchor="w")
        ttk.Label(frame, text="选择 GitHub 固件版本，批量烧录 ESP32-C3，并验收 ML307A / ML307C / ML307R / ML307Y").pack(anchor="w", pady=(2, 12))

        options = ttk.Frame(frame)
        options.pack(fill="x")
        self.firmware_var = tk.StringVar(value=str(defaults.firmware_dir))
        self.version_var = tk.StringVar(value=LOCAL_FIRMWARE_LABEL)
        self.esptool_var = tk.StringVar(value=str(defaults.esptool or ""))
        ttk.Label(options, text="GitHub 版本").grid(row=0, column=0, sticky="w")
        self.version_box = ttk.Combobox(options, textvariable=self.version_var, state="readonly", values=(LOCAL_FIRMWARE_LABEL,))
        self.version_box.grid(row=0, column=1, sticky="ew", padx=8)
        self.version_refresh_button = ttk.Button(options, text="刷新版本", command=self.refresh_versions)
        self.version_refresh_button.grid(row=0, column=2)
        ttk.Label(options, text="本地固件").grid(row=1, column=0, sticky="w", pady=(6, 0))
        ttk.Entry(options, textvariable=self.firmware_var).grid(row=1, column=1, sticky="ew", padx=8, pady=(6, 0))
        ttk.Button(options, text="选择目录", command=self.choose_firmware).grid(row=1, column=2, pady=(6, 0))
        ttk.Label(options, text="esptool").grid(row=2, column=0, sticky="w", pady=(6, 0))
        ttk.Entry(options, textvariable=self.esptool_var).grid(row=2, column=1, sticky="ew", padx=8, pady=(6, 0))
        ttk.Button(options, text="自动查找", command=self.fill_esptool).grid(row=2, column=2, pady=(6, 0))
        options.columnconfigure(1, weight=1)

        checks = ttk.Frame(frame)
        checks.pack(fill="x", pady=10)
        self.erase_var = tk.BooleanVar(value=True)
        self.sim_var = tk.BooleanVar(value=True)
        self.network_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(checks, text="全片擦除（量产推荐）", variable=self.erase_var).pack(side="left")
        ttk.Checkbutton(checks, text="要求 SIM 就绪", variable=self.sim_var).pack(side="left", padx=18)
        ttk.Checkbutton(checks, text="要求成功驻网", variable=self.network_var).pack(side="left")

        toolbar = ttk.Frame(frame)
        toolbar.pack(fill="x", pady=(0, 8))
        ttk.Button(toolbar, text="刷新串口", command=self.refresh_ports).pack(side="left")
        self.start_button = ttk.Button(toolbar, text="开始所选设备", command=self.start)
        self.start_button.pack(side="left", padx=8)
        ttk.Label(toolbar, text="可多选；同一批设备并行烧录").pack(side="left")

        self.tree = ttk.Treeview(frame, columns=("port", "desc", "type", "status"), show="headings", height=8, selectmode="extended")
        for key, title, width in (("port", "串口", 80), ("desc", "设备", 400), ("type", "识别", 120), ("status", "结果", 240)):
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor="w")
        self.tree.pack(fill="x")

        ttk.Label(frame, text="运行日志").pack(anchor="w", pady=(12, 4))
        self.log_widget = tk.Text(frame, height=18, wrap="word", state="disabled", font=("Consolas", 9))
        self.log_widget.pack(fill="both", expand=True)
        self.refresh_ports()
        self.root.after(100, self.poll_events)
        self.refresh_versions()

    def choose_firmware(self) -> None:
        value = self.filedialog.askdirectory(initialdir=self.firmware_var.get())
        if value:
            self.firmware_var.set(value)
            self.version_var.set(LOCAL_FIRMWARE_LABEL)

    def refresh_versions(self) -> None:
        if self.running or self.versions_loading:
            return
        self.versions_loading = True
        self.version_refresh_button.configure(state="disabled")
        self.log(f"正在读取 {self.release_store.repository} 的 GitHub Releases")

        def worker() -> None:
            try:
                self.emit("versions", self.release_store.list_versions())
            except Exception as exc:
                self.emit("version_error", str(exc))

        threading.Thread(target=worker, daemon=True).start()

    def fill_esptool(self) -> None:
        try:
            self.esptool_var.set(str(find_esptool()))
        except Exception as exc:
            self.messagebox.showerror(APP_NAME, str(exc))

    def refresh_ports(self) -> None:
        try:
            ports = scan_ports()
        except Exception as exc:
            self.messagebox.showerror(APP_NAME, str(exc))
            return
        self.tree.delete(*self.tree.get_children())
        self.port_rows.clear()
        for item in ports:
            row = self.tree.insert("", "end", values=(item.device, item.description, "ESP32 候选" if item.likely_esp32 else "普通串口", "待处理"))
            self.port_rows[item.device] = row
            if item.likely_esp32:
                self.tree.selection_add(row)

    def emit(self, kind: str, value: Any) -> None:
        self.events.put((kind, value))

    def log(self, value: str) -> None:
        self.emit("log", value)

    def start(self) -> None:
        if self.running:
            return
        selected = [str(self.tree.item(row, "values")[0]) for row in self.tree.selection()]
        if not selected:
            self.messagebox.showinfo(APP_NAME, "请先选择至少一个串口")
            return
        try:
            esptool = find_esptool(self.esptool_var.get() or None)
        except Exception as exc:
            self.messagebox.showerror(APP_NAME, str(exc))
            return
        release = self.release_rows.get(self.version_var.get())
        local_firmware = Path(self.firmware_var.get())
        self.running = True
        self.start_button.configure(state="disabled")
        self.version_refresh_button.configure(state="disabled")
        erase = self.erase_var.get()
        require_sim = self.sim_var.get()
        require_network = self.network_var.get()
        for port in selected:
            self.tree.set(self.port_rows[port], "status", "处理中")

        def worker() -> None:
            try:
                if release is not None:
                    bundle = self.release_store.resolve(release.tag, self.log)
                else:
                    self.log(f"正在读取本地固件：{local_firmware}")
                    bundle = FirmwareBundle.load(local_firmware)
                self.log(f"本批使用固件 {bundle.version}")
            except Exception as exc:
                self.emit("prepare_error", (selected, str(exc)))
                return
            record_root = application_root() / "records"
            passed = 0
            with ThreadPoolExecutor(max_workers=min(4, len(selected))) as pool:
                futures = {
                    pool.submit(
                        produce_one, port, bundle, esptool, record_root,
                        erase, False, require_sim,
                        require_network, 120, self.log,
                    ): port for port in selected
                }
                for future in as_completed(futures):
                    port = futures[future]
                    ok, status, error = future.result()
                    passed += int(ok)
                    modem = (status.get("modem") or {}).get("family", "")
                    self.emit("status", (port, "PASS " + modem if ok else "FAIL " + error))
            self.emit("done", (passed, len(selected)))

        threading.Thread(target=worker, daemon=True).start()

    def poll_events(self) -> None:
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "log":
                    self.log_widget.configure(state="normal")
                    self.log_widget.insert("end", str(value) + "\n")
                    self.log_widget.see("end")
                    self.log_widget.configure(state="disabled")
                elif kind == "status":
                    port, status = value
                    if port in self.port_rows:
                        self.tree.set(self.port_rows[port], "status", status)
                elif kind == "versions":
                    versions = list(value)
                    current = self.version_var.get()
                    self.release_rows = {item.label: item for item in versions}
                    labels = list(self.release_rows) + [LOCAL_FIRMWARE_LABEL]
                    self.version_box.configure(values=labels)
                    self.version_var.set(current if current in labels else labels[0])
                    self.versions_loading = False
                    if not self.running:
                        self.version_refresh_button.configure(state="normal")
                    self.log_widget.configure(state="normal")
                    self.log_widget.insert("end", f"已读取 {len(versions)} 个可烧录版本，当前选择 {self.version_var.get()}\n")
                    self.log_widget.see("end")
                    self.log_widget.configure(state="disabled")
                elif kind == "version_error":
                    self.versions_loading = False
                    self.version_var.set(LOCAL_FIRMWARE_LABEL)
                    self.version_box.configure(values=(LOCAL_FIRMWARE_LABEL,))
                    if not self.running:
                        self.version_refresh_button.configure(state="normal")
                    self.log_widget.configure(state="normal")
                    self.log_widget.insert("end", f"在线版本读取失败，可继续使用本地固件：{value}\n")
                    self.log_widget.see("end")
                    self.log_widget.configure(state="disabled")
                elif kind == "prepare_error":
                    ports, message = value
                    for port in ports:
                        if port in self.port_rows:
                            self.tree.set(self.port_rows[port], "status", "未开始")
                    self.running = False
                    self.start_button.configure(state="normal")
                    self.version_refresh_button.configure(state="normal")
                    self.messagebox.showerror(APP_NAME, f"固件准备失败：{message}")
                elif kind == "done":
                    passed, total = value
                    self.running = False
                    self.start_button.configure(state="normal")
                    self.version_refresh_button.configure(state="normal")
                    self.messagebox.showinfo(APP_NAME, f"本批完成：{passed}/{total} 台通过\n记录已写入 factory/records")
        except queue.Empty:
            pass
        self.root.after(100, self.poll_events)


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    root = application_root()
    parser = argparse.ArgumentParser(description=APP_NAME)
    parser.add_argument("--list", action="store_true", help="列出串口")
    parser.add_argument("--ports", nargs="+", help="量产指定串口，例如 COM3 COM4")
    parser.add_argument("--all", action="store_true", help="量产所有识别为 ESP32 的串口")
    parser.add_argument("--firmware-dir", type=Path, default=root / "dist")
    parser.add_argument("--github-repo", default=DEFAULT_GITHUB_REPOSITORY, help="在线固件仓库 owner/repository")
    parser.add_argument("--version", help="GitHub Release 版本号，例如 v1.3.0 或 latest")
    parser.add_argument("--list-versions", action="store_true", help="列出 GitHub 可烧录版本")
    parser.add_argument("--firmware-cache", type=Path, default=firmware_cache_root())
    parser.add_argument("--esptool")
    parser.add_argument("--records", type=Path, default=root / "records")
    parser.add_argument("--skip-flash", action="store_true", help="只执行工厂验收")
    parser.add_argument("--keep-data", action="store_true", help="烧录前不擦除全片")
    parser.add_argument("--require-sim", action="store_true")
    parser.add_argument("--require-network", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=120)
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(argv)
    release_store = GitHubReleaseStore(args.github_repo, args.firmware_cache)
    if args.list_versions:
        for item in release_store.list_versions():
            print(f"{item.tag}\t{item.name}\t{item.asset_name}")
        return 0
    if args.list:
        for item in scan_ports():
            print(f"{item.device}\t{'ESP32' if item.likely_esp32 else '-'}\t{item.description}")
        return 0
    ports = list(args.ports or [])
    if args.all:
        ports.extend(item.device for item in scan_ports() if item.likely_esp32)
    ports = list(dict.fromkeys(ports))
    if not ports:
        import tkinter as tk
        root = tk.Tk()
        ProductionApp(root, args)
        root.mainloop()
        return 0

    if args.skip_flash:
        bundle = None
    elif args.version:
        bundle = release_store.resolve(args.version, print)
    else:
        bundle = FirmwareBundle.load(args.firmware_dir)
    esptool = find_esptool(args.esptool)
    passed = 0
    with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, len(ports)))) as pool:
        futures = {
            pool.submit(
                produce_one, port, bundle, esptool, args.records,
                not args.keep_data, args.skip_flash, args.require_sim,
                args.require_network, args.timeout, print,
            ): port for port in ports
        }
        for future in as_completed(futures):
            ok, _, _ = future.result()
            passed += int(ok)
    print(f"完成：{passed}/{len(ports)} 台通过")
    return 0 if passed == len(ports) else 2


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Copy Arduino output and create a hash-verified production bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
from datetime import datetime, timezone
from pathlib import Path


IMAGE_LAYOUT = (
    ("0x0", "code.ino.bootloader.bin"),
    ("0x8000", "code.ino.partitions.bin"),
    ("0xe000", "boot_app0.bin"),
    ("0x10000", "code.ino.bin"),
)

PARTITION_ENTRY = struct.Struct("<HBBII16sI")
EXPECTED_PARTITIONS = {
    "nvs": (0x01, 0x02, 0x9000, 0x5000),
    "otadata": (0x01, 0x00, 0xE000, 0x2000),
    "app0": (0x00, 0x10, 0x10000, 0x1F0000),
    "app1": (0x00, 0x11, 0x200000, 0x1F0000),
    "spiffs": (0x01, 0x82, 0x3F0000, 0x10000),
}


def file_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_partition_table(payload: bytes) -> dict[str, tuple[int, int, int, int]]:
    partitions = {}
    for cursor in range(0, len(payload) - PARTITION_ENTRY.size + 1, PARTITION_ENTRY.size):
        magic, part_type, subtype, offset, size, raw_label, _flags = PARTITION_ENTRY.unpack_from(
            payload, cursor
        )
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA:
            raise RuntimeError(f"分区表条目损坏：offset 0x{cursor:x}, magic 0x{magic:04x}")
        label = raw_label.split(b"\0", 1)[0].decode("ascii", "strict")
        if not label or label in partitions:
            raise RuntimeError(f"分区表标签无效或重复：{label!r}")
        partitions[label] = (part_type, subtype, offset, size)
    return partitions


def validate_partition_table(path: Path) -> None:
    partitions = parse_partition_table(path.read_bytes())
    for label, expected in EXPECTED_PARTITIONS.items():
        actual = partitions.get(label)
        if actual != expected:
            raise RuntimeError(
                f"分区表不符合量产要求：{label} 应为 {expected}，实际为 {actual}"
            )


def source_version(repo: Path) -> str:
    text = (repo / "code" / "globals.h").read_text(encoding="utf-8")
    match = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not match:
        raise RuntimeError("无法从 code/globals.h 读取固件版本")
    return match.group(1)


def git_commit(repo: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def build_manifest(repo: Path, build: Path, output: Path) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    images = []
    merged = build / "code.ino.merged.bin"
    for offset, filename in IMAGE_LAYOUT:
        source = build / filename
        target = output / filename
        if source.is_file():
            shutil.copy2(source, target)
        elif filename == "boot_app0.bin" and merged.is_file():
            # Arduino CLI does not always copy this common image to --output-dir.
            # The merged image already contains it at 0xe000.
            with merged.open("rb") as payload:
                payload.seek(0xE000)
                target.write_bytes(payload.read(0x2000))
        else:
            raise RuntimeError(f"Arduino 编译产物缺失：{source}")
        images.append(
            {"offset": offset, "file": filename, "size": target.stat().st_size, "sha256": file_hash(target)}
        )
    if merged.is_file():
        shutil.copy2(merged, output / merged.name)
    validate_partition_table(output / "code.ino.partitions.bin")
    manifest = {
        "schema": 1,
        "product": "sms-forwarder-board",
        "version": source_version(repo),
        "sourceCommit": git_commit(repo),
        "builtAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "chip": "esp32c3",
        "flashSize": "4MB",
        "fqbn": "esp32:esp32:makergo_c3_supermini",
        "partitionScheme": "dual_ota_4mb",
        "otaCapable": True,
        "storage": {"type": "littlefs", "offset": "0x3f0000", "size": 65536},
        "supportedModems": ["ML307A", "ML307C", "ML307R", "ML307Y"],
        "images": images,
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    manifest = build_manifest(args.repo.resolve(), args.build.resolve(), args.output.resolve())
    print(json.dumps(manifest, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

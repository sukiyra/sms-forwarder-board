#!/usr/bin/env python3
"""Generate an editable KiCad 10, net-labelled schematic from the PCB netlist."""

from __future__ import annotations

import math
import os
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

import pcbnew


ROOT = Path(__file__).resolve().parents[2]
HW = ROOT / "hardware"
PROJECT = HW / "sms_forwarder_ml307a"
BOARD = PROJECT / "sms_forwarder_ml307a.kicad_pcb"
SCHEMATIC = PROJECT / "sms_forwarder_ml307a.kicad_sch"
PDF = HW / "drawings" / "schematic.pdf"
SYMBOL_LIBRARY = HW / "libs" / "SMS_Board.kicad_sym"


def find_kicad_cli() -> Path:
    if configured := os.environ.get("KICAD_BIN"):
        directory = Path(configured)
        return directory / ("kicad-cli.exe" if os.name == "nt" else "kicad-cli")
    executable = shutil.which("kicad-cli") or shutil.which("kicad-cli.exe")
    if executable:
        return Path(executable).resolve()
    sibling = Path(sys.executable).resolve().parent / ("kicad-cli.exe" if os.name == "nt" else "kicad-cli")
    if sibling.exists():
        return sibling
    if local_app_data := os.environ.get("LOCALAPPDATA"):
        candidate = Path(local_app_data) / "Programs" / "KiCad" / "10.0" / "bin" / "kicad-cli.exe"
        if candidate.exists():
            return candidate
    raise RuntimeError("KiCad 10 was not found. Set KICAD_BIN to the KiCad bin directory.")


KICAD = find_kicad_cli()


def uid() -> str:
    return str(uuid.uuid4())


def q(value: object) -> str:
    return str(value).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "{newline}")


def fmt(value: float) -> str:
    return f"{value:.3f}".rstrip("0").rstrip(".")


def pin_rows(pins: list[tuple[str, str]]) -> tuple[list[tuple[str, str, float]], list[tuple[str, str, float]], float]:
    half = math.ceil(len(pins) / 2)
    left_raw = pins[:half]
    right_raw = pins[half:]
    rows = max(len(left_raw), len(right_raw), 2)
    height = max(7.62, (rows - 1) * 2.54 + 5.08)
    top = (rows - 1) * 1.27
    left = [(number, net, top - index * 2.54) for index, (number, net) in enumerate(left_raw)]
    right = [(number, net, top - index * 2.54) for index, (number, net) in enumerate(right_raw)]
    return left, right, height


def lib_symbol(ref: str, value: str, footprint: str, pins: list[tuple[str, str]]) -> str:
    left, right, height = pin_rows(pins)
    symbol_id = f"SMS_Board:{ref}"
    lines = [
        f'    (symbol "{q(symbol_id)}"',
        "      (pin_names (offset 1.016))",
        "      (exclude_from_sim no)",
        "      (in_bom yes)",
        "      (on_board yes)",
        f'      (property "Reference" "{q(ref[0] if ref else "U")}" (at 0 {fmt(height / 2 + 2.54)} 0) (effects (font (size 1.27 1.27))))',
        f'      (property "Value" "{q(value)}" (at 0 {fmt(-height / 2 - 2.54)} 0) (effects (font (size 1.27 1.27))))',
        f'      (property "Footprint" "{q(footprint)}" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
        '      (property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
        '      (property "Description" "Generated from PCB netlist" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))',
        f'      (symbol "{q(ref)}_0_1"',
        f"        (rectangle (start -5.08 {fmt(height / 2)}) (end 5.08 {fmt(-height / 2)})",
        "          (stroke (width 0.254) (type default)) (fill (type background)))",
        "      )",
        f'      (symbol "{q(ref)}_1_1"',
    ]
    for number, net, y in left:
        lines.extend([
            f"        (pin passive line (at -7.62 {fmt(y)} 0) (length 2.54)",
            f'          (name "{q(net)}" (effects (font (size 1 1))))',
            f'          (number "{q(number)}" (effects (font (size 1 1)))))',
        ])
    for number, net, y in right:
        lines.extend([
            f"        (pin passive line (at 7.62 {fmt(y)} 180) (length 2.54)",
            f'          (name "{q(net)}" (effects (font (size 1 1))))',
            f'          (number "{q(number)}" (effects (font (size 1 1)))))',
        ])
    lines.extend(["      )", "      (embedded_fonts no)", "    )"])
    return "\n".join(lines)


def label(net: str, x: float, y: float, angle: int) -> str:
    justify = "left bottom" if angle == 0 else "right bottom"
    return (
        f'  (label "{q(net)}" (at {fmt(x)} {fmt(y)} {angle}) '
        f'(effects (font (size 0.9 0.9)) (justify {justify})) (uuid {uid()}))'
    )


def instance(
    root_uuid: str,
    ref: str,
    value: str,
    footprint: str,
    pins: list[tuple[str, str]],
    dnp: bool,
    x: float,
    y: float,
) -> tuple[str, list[str]]:
    left, right, height = pin_rows(pins)
    symbol_uuid = uid()
    pin_uuids = {number: uid() for number, _ in pins}
    lines = [
        "  (symbol",
        f'    (lib_id "SMS_Board:{q(ref)}")',
        f"    (at {fmt(x)} {fmt(y)} 0)",
        "    (unit 1)",
        "    (exclude_from_sim no)",
        "    (in_bom yes)",
        "    (on_board yes)",
        f"    (dnp {'yes' if dnp else 'no'})",
        f"    (uuid {symbol_uuid})",
        f'    (property "Reference" "{q(ref)}" (at {fmt(x)} {fmt(y - height / 2 - 2.54)} 0) (effects (font (size 1.27 1.27))))',
        f'    (property "Value" "{q(value)}" (at {fmt(x)} {fmt(y + height / 2 + 2.54)} 0) (effects (font (size 1.27 1.27))))',
        f'    (property "Footprint" "{q(footprint)}" (at {fmt(x)} {fmt(y)} 0) (effects (font (size 1.27 1.27)) (hide yes)))',
        f'    (property "Datasheet" "~" (at {fmt(x)} {fmt(y)} 0) (effects (font (size 1.27 1.27)) (hide yes)))',
        f'    (property "Description" "Generated from PCB netlist" (at {fmt(x)} {fmt(y)} 0) (effects (font (size 1.27 1.27)) (hide yes)))',
    ]
    for number, _ in pins:
        lines.append(f'    (pin "{q(number)}" (uuid {pin_uuids[number]}))')
    lines.extend([
        "    (instances",
        '      (project "sms_forwarder_ml307a"',
        f'        (path "/{root_uuid}" (reference "{q(ref)}") (unit 1))',
        "      )",
        "    )",
        "  )",
    ])
    labels: list[str] = []
    for _, net, py in left:
        labels.append(label(net, x - 7.62, y - py, 0))
    for _, net, py in right:
        labels.append(label(net, x + 7.62, y - py, 180))
    return "\n".join(lines), labels


def component_data(board: pcbnew.BOARD) -> list[dict]:
    rows = []
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        value = fp.GetValue()
        item_name = fp.GetFPID().GetLibItemName()
        if (HW / "libs" / "SMS_Forwarder.pretty" / f"{item_name}.kicad_mod").exists():
            library = "SMS_Forwarder"
        elif ref.startswith("R"):
            library = "Resistor_SMD"
        elif ref.startswith("C"):
            library = "Capacitor_SMD"
        elif ref.startswith("SW"):
            library = "Button_Switch_SMD"
        elif ref == "D1":
            library = "Diode_SMD"
        elif ref.startswith("D"):
            library = "LED_SMD"
        elif ref.startswith("TP"):
            library = "TestPoint"
        else:
            library = "SMS_Forwarder"
        footprint = f"{library}:{item_name}"
        pin_nets: dict[str, str] = {}
        for pad in fp.Pads():
            number = str(pad.GetNumber())
            net = pad.GetNetname()
            if not number or not net:
                continue
            if number in pin_nets and pin_nets[number] != net:
                raise RuntimeError(f"{ref} pad {number} has conflicting nets")
            pin_nets[number] = net
        if not pin_nets:
            continue
        pins = sorted(pin_nets.items(), key=lambda item: (not item[0].isdigit(), int(item[0]) if item[0].isdigit() else item[0]))
        rows.append({
            "ref": ref,
            "value": value,
            "footprint": footprint,
            "pins": pins,
            "dnp": fp.IsDNP(),
        })

    def sort_key(row: dict):
        ref = row["ref"]
        prefix = "".join(ch for ch in ref if ch.isalpha())
        number = int("".join(ch for ch in ref if ch.isdigit()) or 0)
        priority = {"U": 0, "J": 1, "F": 2, "L": 3, "SW": 4, "D": 5, "R": 6, "C": 7, "TP": 8}
        return priority.get(prefix, 99), number

    return sorted(rows, key=sort_key)


def pack(components: list[dict]) -> list[tuple[dict, float, float]]:
    columns = [30.48, 91.44, 152.4, 213.36, 274.32, 335.28, 396.24]
    x_index = 0
    y = 30.48
    packed = []
    for row in components:
        _, _, height = pin_rows(row["pins"])
        required = height + 10.16
        if y + required > 277.0:
            x_index += 1
            y = 30.48
        if x_index >= len(columns):
            raise RuntimeError("A2 schematic page is full")
        packed.append((row, columns[x_index], y + height / 2))
        y += required
    return packed


def generate() -> None:
    board = pcbnew.LoadBoard(str(BOARD))
    components = component_data(board)
    root_uuid = uid()
    libraries = [lib_symbol(row["ref"], row["value"], row["footprint"], row["pins"]) for row in components]
    instances: list[str] = []
    labels: list[str] = []
    for row, x, y in pack(components):
        symbol, symbol_labels = instance(
            root_uuid,
            row["ref"],
            row["value"],
            row["footprint"],
            row["pins"],
            row["dnp"],
            x,
            y,
        )
        instances.append(symbol)
        labels.extend(symbol_labels)

    contents = [
        "(kicad_sch", "  (version 20250114)", '  (generator "eeschema")',
        '  (generator_version "10.0")', f"  (uuid {root_uuid})", '  (paper "A3")',
        "  (title_block", '    (title "SMS Forwarder Board — ESP32-C3 + ML307A")',
        '    (date "2026-09-07")', '    (rev "1.0")', '    (company "sukiyra")',
        '    (comment 1 "CERN-OHL-P-2.0 open hardware")',
        '    (comment 2 "5 V / 2 A input; 4-layer JLC04161H-7628")',
        '    (comment 3 "ML307A 94-pin PCB variant")',
        '    (comment 4 "Firmware: TX GPIO3, RX GPIO4, modem EN GPIO5, LED GPIO8")',
        "  )", "  (lib_symbols", *libraries, "  )", *labels, *instances,
        "  (sheet_instances", '    (path "/" (page "1"))', "  )", ")", "",
    ]
    SCHEMATIC.write_text("\n".join(contents), encoding="utf-8")
    external_symbols = []
    for item in libraries:
        external_symbols.append(item.replace('(symbol "SMS_Board:', '(symbol "', 1))
    SYMBOL_LIBRARY.write_text(
        "(kicad_symbol_lib\n  (version 20250114)\n  (generator kicad_symbol_editor)\n"
        + "\n".join(external_symbols)
        + "\n)\n",
        encoding="utf-8",
    )
    (PROJECT / "sym-lib-table").write_text(
        '(sym_lib_table\n'
        '  (lib (name "SMS_Forwarder")(type "KiCad")(uri "${KIPRJMOD}/../libs/SMS_Forwarder.kicad_sym")(options "")(descr ""))\n'
        '  (lib (name "SMS_Board")(type "KiCad")(uri "${KIPRJMOD}/../libs/SMS_Board.kicad_sym")(options "")(descr "Generated schematic symbols"))\n'
        ')\n',
        encoding="utf-8",
    )
    PDF.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(KICAD), "sch", "upgrade", "--force", str(SCHEMATIC)], check=True, cwd=PROJECT)
    subprocess.run([str(KICAD), "sch", "export", "pdf", "--output", str(PDF), str(SCHEMATIC)], check=True, cwd=PROJECT)


if __name__ == "__main__":
    generate()

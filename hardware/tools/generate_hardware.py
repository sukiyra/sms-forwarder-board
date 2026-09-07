#!/usr/bin/env python3
"""Generate the KiCad PCB and release manufacturing data for SMS Forwarder Board.

The script uses KiCad's bundled pcbnew Python module.  It deliberately keeps the
board source reproducible: component placement, net assignment, copper, and the
documentation BOM all come from this file.
"""

from __future__ import annotations

import csv
import json
import os
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HW = ROOT / "hardware"
PROJECT = HW / "sms_forwarder_ml307a"
LIB = HW / "libs" / "SMS_Forwarder.pretty"


def find_kicad_bin() -> Path:
    if configured := os.environ.get("KICAD_BIN"):
        return Path(configured)
    executable = shutil.which("kicad-cli") or shutil.which("kicad-cli.exe")
    if executable:
        return Path(executable).resolve().parent
    candidates = [Path(sys.executable).resolve().parent]
    if local_app_data := os.environ.get("LOCALAPPDATA"):
        candidates.append(Path(local_app_data) / "Programs" / "KiCad" / "10.0" / "bin")
    for candidate in candidates:
        if (candidate / "kicad-cli.exe").exists() or (candidate / "kicad-cli").exists():
            return candidate
    raise RuntimeError("KiCad 10 was not found. Set KICAD_BIN to the KiCad bin directory.")


KICAD = find_kicad_bin()

sys.path.insert(0, str(KICAD / "Lib" / "site-packages"))
if hasattr(os, "add_dll_directory"):
    os.add_dll_directory(str(KICAD))
import pcbnew  # type: ignore  # noqa: E402

LOG_FILE = ROOT / ".build" / "hardware-generator-progress.txt"


def log_progress(message: str):
    LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
    with LOG_FILE.open("a", encoding="utf-8") as f:
        f.write(message + "\n")


def mm(value: float) -> int:
    return pcbnew.FromMM(value)


def pt(x: float, y: float):
    return pcbnew.VECTOR2I(mm(x), mm(y))


def angle(degrees: float):
    return pcbnew.EDA_ANGLE(degrees, pcbnew.DEGREES_T)


KICAD_FP_ROOT = KICAD.parent / "share" / "kicad" / "footprints"


@dataclass(frozen=True)
class Part:
    refs: str
    qty: int
    value: str
    description: str
    package: str
    manufacturer: str
    mpn: str
    lcsc: str
    assembly: str = "SMT"
    notes: str = ""


BOM = [
    Part("U1", 1, "ESP32-C3-MINI-1-N4", "Wi-Fi/BLE MCU module, 4 MB flash", "SMD 13.2x16.6 mm", "Espressif", "ESP32-C3-MINI-1-N4", "C2838502"),
    Part("U2", 1, "ML307A-DSLN", "LTE Cat.1 + GNSS modem; DCLN may be fitted when GNSS is not needed", "LGA-94 17.7x15.8 mm", "China Mobile IoT", "ML307A-DSLN", "C5375091", notes="Alternative: ML307A-DCLN / C5362285. Do not fit ML307C/Y on this PCB revision."),
    Part("U3", 1, "SY8205FCC", "5 A synchronous buck, set to 3.816 V", "SOIC-8-EP", "Silergy", "SY8205FCC", "C111875"),
    Part("U4", 1, "AP2112K-3.3", "600 mA 3.3 V LDO", "SOT-23-5", "Diodes Incorporated", "AP2112K-3.3TRG1", "C51118"),
    Part("U5", 1, "TXU0202", "Fixed-direction dual UART level translator", "VSSOP-8", "Texas Instruments", "TXU0202DCUR", "C5186957"),
    Part("U6", 1, "USBLC6-2SC6", "USB 2.0 ESD protection", "SOT-23-6", "UMW", "USBLC6-2SC6", "C2687116"),
    Part("U7", 1, "SRV05-4", "Four-line SIM ESD protection", "SOT-23-6", "TECH PUBLIC", "SRV05-4", "C558418"),
    Part("J1", 1, "USB-A male", "Right-angle USB 2.0 plug", "THT", "Korean Hroparts Elec", "AM90", "C404965", assembly="THT/manual"),
    Part("J2", 1, "Nano SIM", "Hinged nano-SIM socket", "SMD 1.45 mm", "XYECONN", "NANO SIM 6PIN 1.45H", "C53200251"),
    Part("J3,J4", 2, "U.FL", "50 ohm LTE MAIN and optional GNSS antenna connectors", "SMD", "Hirose", "U.FL-R-SMT-1(10)", "C88373"),
    Part("F1", 1, "2 A PTC", "USB input resettable fuse", "1206", "TECHFUSE", "SL1206200", "C70167"),
    Part("L1", 1, "4.7 uH", "6 A shielded power inductor", "7.0x6.6 mm", "XR", "XR0630-4R7M", "C5307631"),
    Part("C5,C6", 2, "220 uF 6.3 V", "Low-ESR polymer modem bulk capacitor", "D5xL5.3 mm", "Nippon Chemi-Con", "APXF6R3ARA221ME61G", "C2162607"),
    Part("C1,C2", 2, "22 uF 10 V", "Buck input ceramic capacitor", "0805", "Samsung", "CL21A226MPQNNNE", "C45783"),
    Part("C3,C4", 2, "47 uF 6.3 V", "Buck output ceramic capacitor", "0805", "Samsung", "CL21A476MQYNNNE", "C96123"),
    Part("C11,C12", 2, "10 uF 10 V", "LDO input/output capacitor", "0603", "Samsung", "CL10A106KP8NNNC", "C19702"),
    Part("C8,C13,C14,C16,C17,C18", 6, "100 nF", "Local bypass capacitor", "0402", "Samsung", "CL05B104KO5NNNC", "C1525"),
    Part("C9", 1, "10 nF", "Buck soft-start capacitor", "0402", "Samsung", "CL05B103KB5NNNC", "C15195"),
    Part("C10", 1, "1 uF", "Buck VCC bypass capacitor", "0402", "Samsung", "CL05A105KA5NNNC", "C52923"),
    Part("C15", 1, "1 uF", "ESP32 enable delay capacitor", "0402", "Samsung", "CL05A105KA5NNNC", "C52923"),
    Part("C7", 1, "22 pF", "Buck feed-forward capacitor", "0402", "Samsung", "CL05C220JB5NNNC", "C1555"),
    Part("C19,C20,C21,C22", 4, "33 pF", "SIM line RF filter capacitor", "0402", "Samsung", "CL05C330JB5NNNC", "C1562"),
    Part("C23,C24,C25,C26", 4, "DNP", "RF pi matching shunt capacitors", "0402", "-", "DNP", "", assembly="DNP", notes="Select after antenna/VNA validation."),
    Part("R1", 1, "536 k", "Buck upper feedback resistor, 0.1%", "0402", "UNI-ROYAL", "0402WGF5363TCE", "C72998"),
    Part("R2", 1, "100 k", "Buck lower feedback resistor, 0.1% preferred", "0402", "UNI-ROYAL", "0402WGF1003TCE", "C25741"),
    Part("R3,R4,R6,R13", 4, "10 k", "Pull-up resistor", "0402", "UNI-ROYAL", "0402WGF1002TCE", "C25744"),
    Part("R7", 1, "4.7 k", "ML307 PWR_ON active-low auto-start", "0402", "UNI-ROYAL", "0402WGF4701TCE", "C25900"),
    Part("R8,R18", 2, "100 k", "Enable pull-down resistor", "0402", "UNI-ROYAL", "0402WGF1003TCE", "C25741"),
    Part("R9,R10,R11,R12,R16,R17", 6, "22 R", "SIM/USB series damping resistor", "0402", "UNI-ROYAL", "0402WGF220JTCE", "C25092"),
    Part("R14,R15", 2, "0 R", "RF pi matching series link", "0402", "UNI-ROYAL", "0402WGF0000TCE", "C17168"),
    Part("R5", 1, "1 k", "Status LED current limiting resistor", "0402", "UNI-ROYAL", "0402WGF1001TCE", "C11702"),
    Part("D1", 1, "SMF5.0A", "USB VBUS transient suppressor", "SOD-123FL", "JSCJ", "SMF5.0A", "C1509112"),
    Part("D2", 1, "Green LED", "Active-low status indicator", "0603", "XINGLIGHT", "XL-1608UGC-04", "C965804"),
    Part("SW1,SW2", 2, "Tact switch", "RESET and BOOT buttons", "3.0x2.5 mm SMD", "C&K", "KMR221GLFS", "C72443"),
    Part("TP1,TP2,TP3,TP4,TP5,TP6", 6, "Test pad", "Factory and service test points", "1.0 mm pad", "-", "PCB pad", "", assembly="PCB"),
]


def load_fp(library: Path, name: str):
    fp = pcbnew.FootprintLoad(str(library), name)
    if fp is None:
        raise RuntimeError(f"Cannot load footprint {library}:{name}")
    return fp


def standard_fp(library: str, name: str):
    return load_fp(KICAD_FP_ROOT / f"{library}.pretty", name)


def generate_board() -> Path:
    log_progress("generate_board:start")
    PROJECT.mkdir(parents=True, exist_ok=True)
    board = pcbnew.BOARD()
    log_progress("board:created")
    board.SetCopperLayerCount(4)
    log_progress("board:layers")
    ds = board.GetDesignSettings()
    ds.m_MinClearance = mm(0.15)
    ds.m_TrackMinWidth = mm(0.15)
    ds.m_ViasMinSize = mm(0.45)
    ds.m_MinThroughDrill = mm(0.2)
    net_settings = ds.m_NetSettings
    for class_name, width, clearance, via_diameter, via_drill in [
        ("Signal", 0.20, 0.18, 0.60, 0.30),
        ("LOGIC_POWER", 0.25, 0.18, 0.60, 0.30),
        ("USB_DIFF", 0.20, 0.18, 0.60, 0.30),
        ("RF_50R", 0.38, 0.25, 0.70, 0.30),
        ("Power", 0.80, 0.20, 0.80, 0.40),
        ("MODEM_VBAT", 1.50, 0.25, 1.00, 0.50),
        ("GND", 0.50, 0.20, 0.65, 0.30),
    ]:
        nc = pcbnew.NETCLASS(class_name)
        nc.SetClearance(mm(clearance)); nc.SetTrackWidth(mm(width))
        nc.SetViaDiameter(mm(via_diameter)); nc.SetViaDrill(mm(via_drill))
        if class_name == "USB_DIFF":
            nc.SetDiffPairWidth(mm(0.20)); nc.SetDiffPairGap(mm(0.20))
        net_settings.SetNetclass(class_name, nc)

    nets = {}
    for name in [
        "GND", "+5V_RAW", "+5V", "+3V3", "+3V8_MODEM", "+1V8_MODEM",
        "MODEM_EN", "ESP_EN", "ESP_BOOT", "LED_CTRL", "LED_A", "LEVEL_OE",
        "ESP_TX_3V3", "ESP_RX_3V3", "MODEM_RX_1V8", "MODEM_TX_1V8",
        "USB_CONN_DP", "USB_CONN_DM", "USB_DP_PRE", "USB_DM_PRE", "USB_DP", "USB_DM",
        "SIM_VCC_M", "SIM_VCC", "SIM_DATA_M", "SIM_RST_M", "SIM_CLK_M",
        "SIM_DATA", "SIM_RST", "SIM_CLK", "MODEM_RESET", "MODEM_PWRKEY",
        "LTE_ANT", "LTE_CONN", "GNSS_ANT", "GNSS_CONN",
        "BUCK_BS", "BUCK_LX", "BUCK_FB", "BUCK_SS",
    ]:
        net = pcbnew.NETINFO_ITEM(board, name)
        board.Add(net)
        nets[name] = net
    for name in nets:
        net_settings.SetNetclassPatternAssignment(name, "Signal")
    for name in ["+5V_RAW", "+5V"]:
        net_settings.SetNetclassPatternAssignment(name, "Power")
    for name in ["+3V3", "+1V8_MODEM"]:
        net_settings.SetNetclassPatternAssignment(name, "LOGIC_POWER")
    net_settings.SetNetclassPatternAssignment("+3V8_MODEM", "MODEM_VBAT")
    net_settings.SetNetclassPatternAssignment("GND", "GND")
    for name in ["USB_CONN_DP", "USB_CONN_DM", "USB_DP_PRE", "USB_DM_PRE", "USB_DP", "USB_DM"]:
        net_settings.SetNetclassPatternAssignment(name, "USB_DIFF")
    for name in ["LTE_ANT", "LTE_CONN", "GNSS_ANT", "GNSS_CONN"]:
        net_settings.SetNetclassPatternAssignment(name, "RF_50R")
    net_settings.RecomputeEffectiveNetclasses()
    log_progress("board:nets")

    footprints = {}

    def place(ref, value, fp, x, y, rot=0, bottom=False):
        fp.SetReference(ref)
        fp.SetValue(value)
        if ref != "J1" and not ref.startswith("TP"):
            fp.SetAttributes(pcbnew.FP_SMD)
        fp.SetPosition(pt(x, y))
        board.Add(fp)
        if bottom:
            fp.Flip(fp.GetPosition(), False)
        if rot:
            fp.SetOrientation(angle(rot))
        for field in fp.GetFields():
            field.SetVisible(False)
        footprints[ref] = fp
        return fp

    # Core modules and connectors.
    place("U2", "ML307A-DSLN", load_fp(LIB, "LGA-94_L17.7-W15.8_N706-CA-01-S1"), 117, 69)
    log_progress("board:U2")
    place("U1", "ESP32-C3-MINI-1-N4", load_fp(LIB, "WIFIM-SMD_ESP32-C3-MINI-1"), 119, 109, 90, True)
    log_progress("board:U1")
    place("J1", "USB-A MALE", load_fp(LIB, "USB-AM-TH_AM90"), 117, 126.5)
    log_progress("board:J1")
    place("J2", "NANO SIM", load_fp(LIB, "SIM-SMD_NANO-SIM-6PIN-1.45H"), 117, 85, 0, True)
    log_progress("board:J2")
    place("J3", "LTE MAIN", load_fp(LIB, "ANT-SMD_UFL-R-SMT-1-10"), 110, 53)
    place("J4", "GNSS", load_fp(LIB, "ANT-SMD_UFL-R-SMT-1-10"), 101.5, 82.5, 90)

    # Power and interface ICs.
    place("U3", "SY8205FCC", load_fp(LIB, "SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL-EP2.0"), 104.5, 105, 180)
    log_progress("board:U3")
    place("L1", "4.7uH 6A", load_fp(LIB, "IND-SMD_L7.0-W6.6"), 104.5, 93, 90)
    u4 = place("U4", "AP2112K-3.3", load_fp(LIB, "SOT-25-5_L2.9-W1.6-P0.95-LS2.8-BL"), 130, 123.5, 180)
    # The vendor footprint outline runs over C14 after rotation.  Keep the
    # assembly outline on Fab instead of allowing it to be clipped in copper.
    for graphic in u4.GraphicalItems():
        if graphic.GetLayer() == pcbnew.F_SilkS:
            graphic.SetLayer(pcbnew.F_Fab)
    place("U5", "TXU0202", load_fp(LIB, "VSSOP-8_L2.3-W2.0-P0.50-LS3.1-BR"), 130, 72)
    place("U6", "USBLC6-2SC6", load_fp(LIB, "SOT-23-6_L2.9-W1.6-P0.95-LS2.8-BL"), 117, 121, 90, True)
    place("U7", "SRV05-4", load_fp(LIB, "SOT-23-6_L2.9-W1.6-P0.95-LS2.8-BL"), 127, 84, 90, True)
    place("F1", "2A PTC", load_fp(LIB, "F1206"), 127.5, 127, 0)
    place("D1", "SMF5.0A", standard_fp("Diode_SMD", "D_SOD-123F"), 133, 127, 90)
    log_progress("board:core")

    # Helpers for passives.
    def r(ref, value, x, y, rot=0, bottom=False, size="0402"):
        name = "R_0402_1005Metric" if size == "0402" else "R_0603_1608Metric"
        return place(ref, value, standard_fp("Resistor_SMD", name), x, y, rot, bottom)

    def c(ref, value, x, y, rot=0, bottom=False, size="0402"):
        names = {"0402": "C_0402_1005Metric", "0603": "C_0603_1608Metric", "0805": "C_0805_2012Metric"}
        return place(ref, value, standard_fp("Capacitor_SMD", names[size]), x, y, rot, bottom)

    # Modem bulk/output and buck network.
    place("C5", "220uF/6.3V", load_fp(LIB, "CAP-SMD_BD5.0-L5.3-W5.3-LS5.7-FD"), 102.5, 67.5, 90)
    place("C6", "220uF/6.3V", load_fp(LIB, "CAP-SMD_BD5.0-L5.3-W5.3-LS5.7-FD"), 102.5, 75.5, 90)
    c("C3", "47uF", 109.5, 88.5, 90, size="0805")
    c("C4", "47uF", 109.5, 92.5, 90, size="0805")
    c("C1", "22uF", 110, 104, 90, size="0805")
    c("C2", "22uF", 110, 108, 90, size="0805")
    r("R1", "536k 0.1%", 109, 98)
    r("R2", "100k 0.1%", 112, 98)
    c("C7", "22pF", 109, 100)
    c("C8", "100nF", 100.8, 100)
    c("C9", "10nF", 108, 108)
    c("C10", "1uF", 108, 105)
    r("R8", "100k", 108, 111)

    # 3.3 V rail and ESP32 straps.
    c("C11", "10uF", 126, 116, 90, size="0603")
    c("C12", "10uF", 132, 118.5, 90, size="0603")
    c("C13", "100nF", 126, 119, 90)
    c("C14", "100nF", 132, 121.5, 90)
    r("R3", "10k", 109, 113)
    c("C15", "1uF", 109, 115)
    r("R4", "10k", 105, 110)
    place("SW1", "RESET", standard_fp("Button_Switch_SMD", "SW_Tactile_SPST_NO_Straight_CK_PTS636Sx25SMTRLFS"), 105, 120, 90)
    place("SW2", "BOOT", standard_fp("Button_Switch_SMD", "SW_Tactile_SPST_NO_Straight_CK_PTS636Sx25SMTRLFS"), 100.5, 120, 90)
    r("R5", "1k", 101, 108, 90)
    place("D2", "STATUS GREEN", standard_fp("LED_SMD", "LED_0603_1608Metric"), 101, 105.5, 90)

    # UART level translation and modem control.
    c("C16", "100nF", 130, 75)
    c("C17", "100nF", 132, 75)
    c("C18", "100nF", 130, 61)
    r("R6", "10k", 130, 77)
    r("R18", "100k", 132, 77)
    r("R7", "4.7k", 117, 79.5)

    # SIM damping, pull-up, filtering and RF matching.
    r("R9", "22R", 122, 79, 90)
    r("R10", "22R", 124, 79, 90)
    r("R11", "22R", 126, 79, 90)
    r("R12", "22R", 128, 79, 90)
    r("R13", "10k", 131, 87, 90)
    c("C19", "33pF", 122, 88, 90)
    c("C20", "33pF", 124, 88, 90)
    c("C21", "33pF", 126, 88, 90)
    c("C22", "33pF", 128, 88, 90)
    r("R14", "0R", 110, 57, 90)
    c("C23", "DNP", 108.5, 58, 90).SetDNP(True)
    c("C24", "DNP", 111.5, 58, 90).SetDNP(True)
    r("R15", "0R", 105.5, 82.5)
    c("C25", "DNP", 106.5, 80.5).SetDNP(True)
    c("C26", "DNP", 106.5, 84.5).SetDNP(True)
    r("R16", "22R", 113, 119, 90)
    r("R17", "22R", 121, 119, 90)

    # Factory test pads.
    test_points = [
        ("TP1", "+5V", 101.5, 119), ("TP2", "+3V8_MODEM", 101.5, 85),
        ("TP3", "+3V3", 131.5, 123), ("TP4", "GND", 101.5, 122),
        ("TP5", "ESP_TX_3V3", 132, 68), ("TP6", "ESP_RX_3V3", 132, 70),
    ]
    for ref, value, x, y in test_points:
        fp = place(ref, value, standard_fp("TestPoint", "TestPoint_Pad_D1.0mm"), x, y, 0, True)
        fp.SetExcludedFromPosFiles(True)

    def assign(ref, mapping):
        fp = footprints[ref]
        by_number = {}
        for pad in fp.Pads():
            by_number.setdefault(str(pad.GetNumber()), []).append(pad)
        for number, net_name in mapping.items():
            for pad in by_number.get(str(number), []):
                pad.SetNet(nets[net_name])

    gnd_ml = [1, 10, 27, 34, 36, 37, 40, 41, 45, 46, 47, 48, 70, 71, 72, 73, 88, 89, 90, 91, 92, 93, 94]
    assign("U2", {**{n: "GND" for n in gnd_ml}, 2: "GNSS_ANT", 7: "MODEM_PWRKEY", 11: "SIM_DATA_M", 12: "SIM_RST_M", 13: "SIM_CLK_M", 14: "SIM_VCC_M", 17: "MODEM_RX_1V8", 18: "MODEM_TX_1V8", 24: "+1V8_MODEM", 35: "LTE_ANT", 42: "+3V8_MODEM", 43: "+3V8_MODEM"})
    assign("U1", {**{n: "GND" for n in [1, 2, 11, 14, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53]}, 3: "+3V3", 6: "ESP_TX_3V3", 8: "ESP_EN", 18: "ESP_RX_3V3", 19: "MODEM_EN", 22: "LED_CTRL", 23: "ESP_BOOT", 26: "USB_DM", 27: "USB_DP"})
    assign("J1", {1: "+5V_RAW", 2: "USB_CONN_DM", 3: "USB_CONN_DP", 4: "GND", "MH1": "GND", "MH2": "GND"})
    assign("J2", {"C1": "SIM_VCC", "C2": "SIM_RST", "C3": "SIM_CLK", "C5": "GND", "C7": "SIM_DATA", "C6": "GND", 8: "GND", 9: "GND", 10: "GND", 11: "GND"})
    assign("J3", {1: "GND", 2: "LTE_CONN", 3: "GND"})
    assign("J4", {1: "GND", 2: "GNSS_CONN", 3: "GND"})
    assign("U3", {1: "BUCK_BS", 2: "BUCK_LX", 3: "MODEM_EN", 4: "BUCK_SS", 5: "BUCK_FB", 6: "+5V", 7: "+5V", 8: "+5V", 9: "GND"})
    assign("L1", {1: "BUCK_LX", 2: "+3V8_MODEM"})
    assign("U4", {1: "+5V", 2: "GND", 3: "+5V", 5: "+3V3"})
    assign("U5", {1: "MODEM_TX_1V8", 2: "GND", 3: "+3V3", 4: "ESP_RX_3V3", 5: "ESP_TX_3V3", 6: "LEVEL_OE", 7: "+1V8_MODEM", 8: "MODEM_RX_1V8"})
    assign("U6", {1: "USB_CONN_DM", 2: "GND", 3: "USB_CONN_DP", 4: "USB_DP_PRE", 5: "+5V", 6: "USB_DM_PRE"})
    assign("U7", {1: "SIM_DATA", 2: "GND", 3: "SIM_RST", 4: "SIM_CLK", 5: "SIM_VCC", 6: "SIM_VCC"})
    assign("F1", {1: "+5V_RAW", 2: "+5V"})
    assign("D1", {1: "GND", 2: "+5V"})
    for ref in ["C1", "C2", "C11", "C13"]: assign(ref, {1: "+5V", 2: "GND"})
    for ref in ["C3", "C4", "C5", "C6"]: assign(ref, {1: "+3V8_MODEM", 2: "GND"})
    for ref in ["C12", "C14"]: assign(ref, {1: "+3V3", 2: "GND"})
    assign("R1", {1: "+3V8_MODEM", 2: "BUCK_FB"}); assign("R2", {1: "BUCK_FB", 2: "GND"})
    assign("C7", {1: "+3V8_MODEM", 2: "BUCK_FB"}); assign("C8", {1: "BUCK_BS", 2: "BUCK_LX"})
    assign("C9", {1: "BUCK_SS", 2: "GND"}); assign("C10", {1: "+5V", 2: "GND"}); assign("R8", {1: "MODEM_EN", 2: "GND"})
    assign("R3", {1: "+3V3", 2: "ESP_EN"}); assign("C15", {1: "ESP_EN", 2: "GND"}); assign("R4", {1: "+3V3", 2: "ESP_BOOT"})
    assign("SW1", {1: "ESP_EN", 2: "GND"}); assign("SW2", {1: "ESP_BOOT", 2: "GND"})
    assign("R5", {1: "+3V3", 2: "LED_A"}); assign("D2", {1: "LED_CTRL", 2: "LED_A"})
    assign("C16", {1: "+3V3", 2: "GND"}); assign("C17", {1: "+1V8_MODEM", 2: "GND"}); assign("C18", {1: "+1V8_MODEM", 2: "GND"})
    assign("R6", {1: "+1V8_MODEM", 2: "LEVEL_OE"}); assign("R18", {1: "LEVEL_OE", 2: "GND"}); assign("R7", {1: "MODEM_PWRKEY", 2: "GND"})
    sim_pairs = [("R9", "SIM_DATA_M", "SIM_DATA"), ("R10", "SIM_RST_M", "SIM_RST"), ("R11", "SIM_CLK_M", "SIM_CLK"), ("R12", "SIM_VCC_M", "SIM_VCC")]
    for ref, a, b in sim_pairs: assign(ref, {1: a, 2: b})
    assign("R13", {1: "SIM_VCC", 2: "SIM_DATA"})
    for ref, sig in [("C19", "SIM_DATA"), ("C20", "SIM_RST"), ("C21", "SIM_CLK"), ("C22", "SIM_VCC")]: assign(ref, {1: sig, 2: "GND"})
    assign("R14", {1: "LTE_CONN", 2: "LTE_ANT"}); assign("C23", {1: "LTE_CONN", 2: "GND"}); assign("C24", {1: "LTE_ANT", 2: "GND"})
    assign("R15", {1: "GNSS_CONN", 2: "GNSS_ANT"}); assign("C25", {1: "GNSS_CONN", 2: "GND"}); assign("C26", {1: "GNSS_ANT", 2: "GND"})
    assign("R16", {1: "USB_DM_PRE", 2: "USB_DM"}); assign("R17", {1: "USB_DP_PRE", 2: "USB_DP"})
    for ref, net_name, _, _ in test_points: assign(ref, {1: net_name})

    def pad_pos(ref, number):
        for pad in footprints[ref].Pads():
            if str(pad.GetNumber()) == str(number):
                return pad.GetPosition()
        raise KeyError(f"{ref}.{number}")

    def track(net_name, points, layer=pcbnew.F_Cu, width=0.25):
        for a, b in zip(points, points[1:]):
            seg = pcbnew.PCB_TRACK(board)
            seg.SetStart(a if not isinstance(a, tuple) else pt(*a))
            seg.SetEnd(b if not isinstance(b, tuple) else pt(*b))
            seg.SetWidth(mm(width))
            seg.SetLayer(layer)
            seg.SetNet(nets[net_name])
            board.Add(seg)

    def via(net_name, x, y, diameter=0.65, drill=0.3):
        item = pcbnew.PCB_VIA(board)
        item.SetPosition(pt(x, y))
        item.SetWidth(mm(diameter))
        item.SetDrill(mm(drill))
        item.SetNet(nets[net_name])
        board.Add(item)
        return item.GetPosition()

    def connect(ref_a, pad_a, ref_b, pad_b, net_name, layer=pcbnew.F_Cu, width=0.25, waypoints=()):
        track(net_name, [pad_pos(ref_a, pad_a), *[pt(*p) for p in waypoints], pad_pos(ref_b, pad_b)], layer, width)

    # RF: 0.38 mm nominal 50-ohm microstrip on the documented JLC04161H stackup.
    connect("J3", 2, "R14", 1, "LTE_CONN", width=0.38, waypoints=[(110, 55)])
    connect("R14", 2, "U2", 35, "LTE_ANT", width=0.38, waypoints=[(110, 60), (110.4, 61.5)])
    connect("J4", 2, "R15", 1, "GNSS_CONN", width=0.38)
    connect("R15", 2, "U2", 2, "GNSS_ANT", width=0.38, waypoints=[(108, 80), (109, 76.5)])

    # High-current modem rail and local converter connections.
    connect("U3", 2, "L1", 1, "BUCK_LX", width=1.2, waypoints=[(101.5, 101), (101.5, 96)])
    for ref in ["C3", "C4", "C5", "C6"]:
        connect("L1", 2, ref, 1, "+3V8_MODEM", width=1.5, waypoints=[(108, 90), (108, 82)])
    track("+3V8_MODEM", [pad_pos("L1", 2), pt(108, 82), pt(107, 73), pad_pos("U2", 42)], width=2.0)
    track("+3V8_MODEM", [pad_pos("U2", 42), pad_pos("U2", 43)], width=2.0)
    connect("R1", 1, "L1", 2, "+3V8_MODEM", width=0.25, waypoints=[(109, 94)])
    connect("C7", 1, "R1", 1, "+3V8_MODEM")
    connect("U3", 5, "R1", 2, "BUCK_FB", width=0.2, waypoints=[(108, 102), (108, 98)])
    connect("R1", 2, "R2", 1, "BUCK_FB", width=0.2)
    connect("C7", 2, "R1", 2, "BUCK_FB", width=0.2)
    connect("U3", 1, "C8", 1, "BUCK_BS", width=0.25)
    connect("C8", 2, "U3", 2, "BUCK_LX", width=0.3)
    connect("U3", 4, "C9", 1, "BUCK_SS", width=0.2)

    # USB input and 5 V distribution.
    connect("J1", 1, "F1", 1, "+5V_RAW", width=1.5)
    track("+5V", [pad_pos("F1", 2), pt(121.5, 121), pt(121.5, 110), pt(111, 110), pt(111, 105)], width=1.2)
    for ref, padnum in [("U3", 6), ("U3", 7), ("U3", 8), ("C1", 1), ("C2", 1), ("C10", 1)]:
        track("+5V", [pt(111, 105), pad_pos(ref, padnum)], width=0.7)
    track("+5V", [pad_pos("F1", 2), pad_pos("D1", 2)], width=1.0)
    track("+5V", [pad_pos("F1", 2), pt(128.5, 124), pad_pos("U4", 1)], width=0.8)
    connect("U4", 1, "U4", 3, "+5V", width=0.5)
    connect("U4", 1, "C11", 1, "+5V", width=0.5)
    connect("U4", 1, "C13", 1, "+5V", width=0.3)

    # 3.3 V uses a short front rail and a via into the ESP bottom-side pads.
    v3 = via("+3V3", 127, 116)
    track("+3V3", [pad_pos("U4", 5), pad_pos("C12", 1), pad_pos("C14", 1), v3], width=0.7)
    track("+3V3", [v3, pad_pos("U1", 3)], pcbnew.B_Cu, 0.7)
    for ref, pnum in [("R3", 1), ("R4", 1), ("R5", 1)]:
        track("+3V3", [pad_pos("U1", 3), pad_pos(ref, pnum)], pcbnew.B_Cu, 0.35)
    track("+3V3", [v3, via("+3V3", 129, 74)], pcbnew.F_Cu, 0.5)
    connect("U5", 3, "C16", 1, "+3V3", width=0.3)

    # Modem enable and UART.  The two UART directions are kept on separate
    # layers after their translator, then meet the bottom-side ESP pads.
    connect("U2", 24, "C18", 1, "+1V8_MODEM", width=0.4)
    connect("U2", 24, "U5", 7, "+1V8_MODEM", width=0.35, waypoints=[(127, 61), (132, 65), (132, 71.75)])
    connect("U5", 7, "C17", 1, "+1V8_MODEM"); connect("U5", 7, "R6", 1, "+1V8_MODEM")
    connect("R6", 2, "U5", 6, "LEVEL_OE"); connect("R18", 1, "U5", 6, "LEVEL_OE")
    connect("U2", 17, "U5", 8, "MODEM_RX_1V8", width=0.25, waypoints=[(127, 70.1)])
    connect("U2", 18, "U5", 1, "MODEM_TX_1V8", width=0.25, waypoints=[(127, 69)])
    tx_v = via("ESP_TX_3V3", 132, 68); rx_v = via("ESP_RX_3V3", 132, 70)
    track("ESP_TX_3V3", [pad_pos("U5", 5), tx_v], width=0.25)
    track("ESP_RX_3V3", [pad_pos("U5", 4), rx_v], width=0.25)
    track("ESP_TX_3V3", [tx_v, pt(132, 95), pad_pos("U1", 6)], pcbnew.B_Cu, 0.25)
    track("ESP_RX_3V3", [rx_v, pt(131, 96), pad_pos("U1", 18)], pcbnew.B_Cu, 0.25)
    en_v = via("MODEM_EN", 108, 112)
    track("MODEM_EN", [pad_pos("U3", 3), pad_pos("R8", 1), en_v], width=0.25)
    track("MODEM_EN", [en_v, pad_pos("U1", 19)], pcbnew.B_Cu, 0.25)
    connect("U2", 7, "R7", 1, "MODEM_PWRKEY", width=0.25)

    # ESP reset, boot and status LED.
    track("ESP_EN", [pad_pos("U1", 8), pad_pos("R3", 2), pad_pos("C15", 1), pad_pos("SW1", 1)], pcbnew.B_Cu, 0.25)
    track("ESP_BOOT", [pad_pos("U1", 23), pad_pos("R4", 2), pad_pos("SW2", 1)], pcbnew.B_Cu, 0.25)
    track("LED_A", [pad_pos("R5", 2), pad_pos("D2", 2)], pcbnew.B_Cu, 0.25)
    track("LED_CTRL", [pad_pos("U1", 22), pad_pos("D2", 1)], pcbnew.B_Cu, 0.25)

    # USB differential pair on bottom, 0.20 mm width/spacing target.
    track("USB_CONN_DM", [pad_pos("J1", 2), pad_pos("U6", 1)], pcbnew.B_Cu, 0.2)
    track("USB_CONN_DP", [pad_pos("J1", 3), pad_pos("U6", 3)], pcbnew.B_Cu, 0.2)
    track("USB_DM_PRE", [pad_pos("U6", 6), pad_pos("R16", 1)], pcbnew.B_Cu, 0.2)
    track("USB_DP_PRE", [pad_pos("U6", 4), pad_pos("R17", 1)], pcbnew.B_Cu, 0.2)
    track("USB_DM", [pad_pos("R16", 2), pt(116, 113), pad_pos("U1", 26)], pcbnew.B_Cu, 0.2)
    track("USB_DP", [pad_pos("R17", 2), pt(118, 113), pad_pos("U1", 27)], pcbnew.B_Cu, 0.2)

    # SIM signals cross through four vias next to the modem and remain on the
    # bottom layer beside the socket/ESD array.
    for idx, (mpad, rref, mnet, cnet, jpad) in enumerate([
        (11, "R9", "SIM_DATA_M", "SIM_DATA", "C7"),
        (12, "R10", "SIM_RST_M", "SIM_RST", "C2"),
        (13, "R11", "SIM_CLK_M", "SIM_CLK", "C3"),
        (14, "R12", "SIM_VCC_M", "SIM_VCC", "C1"),
    ]):
        vx = 122 + idx * 2
        vy = 78
        vv = via(mnet, vx, vy)
        track(mnet, [pad_pos("U2", mpad), vv], width=0.2)
        track(mnet, [vv, pad_pos(rref, 1)], pcbnew.B_Cu, 0.2)
        track(cnet, [pad_pos(rref, 2), pad_pos("J2", jpad)], pcbnew.B_Cu, 0.2)
    track("SIM_DATA", [pad_pos("R9", 2), pad_pos("R13", 2), pad_pos("C19", 1), pad_pos("U7", 1)], pcbnew.B_Cu, 0.2)
    track("SIM_RST", [pad_pos("R10", 2), pad_pos("C20", 1), pad_pos("U7", 3)], pcbnew.B_Cu, 0.2)
    track("SIM_CLK", [pad_pos("R11", 2), pad_pos("C21", 1), pad_pos("U7", 4)], pcbnew.B_Cu, 0.2)
    track("SIM_VCC", [pad_pos("R12", 2), pad_pos("R13", 1), pad_pos("C22", 1), pad_pos("U7", 5), pad_pos("J2", "C1")], pcbnew.B_Cu, 0.3)

    # Test-pad stubs for power and UART.
    for tp_ref, net_name, origin in [("TP1", "+5V", pt(111, 110)), ("TP2", "+3V8_MODEM", pt(108, 82)), ("TP3", "+3V3", v3)]:
        track(net_name, [origin, pad_pos(tp_ref, 1)], pcbnew.B_Cu if tp_ref != "TP1" else pcbnew.F_Cu, 0.5)
    track("ESP_TX_3V3", [tx_v, pad_pos("TP5", 1)], pcbnew.B_Cu, 0.25)
    track("ESP_RX_3V3", [rx_v, pad_pos("TP6", 1)], pcbnew.B_Cu, 0.25)

    # Give small local ground pours a fixed dog-bone connection to the solid
    # inner ground plane.  These fanouts are present in the DSN so the
    # autorouter keeps signal tracks clear of them.  A 0.30 mm finished drill
    # and 0.60 mm land use a conservative standard-process geometry while
    # avoiding via-in-pad solder wicking on the VSSOP and 0402 footprints.
    gnd_fanouts = [
        ("U4", 2, (130.00, 121.30), pcbnew.F_Cu),
        ("C2", 2, (110.80, 107.05), pcbnew.F_Cu),
        ("C10", 2, (108.48, 105.80), pcbnew.F_Cu),
        ("C16", 2, (130.48, 74.20), pcbnew.F_Cu),
        ("U5", 2, (132.20, 72.25), pcbnew.F_Cu),
        ("C20", 2, (124.00, 86.72), pcbnew.F_Cu),
        ("C21", 2, (126.00, 86.72), pcbnew.F_Cu),
        ("C22", 2, (128.00, 86.72), pcbnew.F_Cu),
        ("U6", 2, (115.00, 121.00), pcbnew.B_Cu),
        ("U7", 2, (124.90, 84.00), pcbnew.B_Cu),
        ("J2", "C5", (118.91, 79.40), pcbnew.B_Cu),
    ]
    for ref, pad_number, target, layer in gnd_fanouts:
        target_pos = via("GND", target[0], target[1], 0.60, 0.30)
        track("GND", [pad_pos(ref, pad_number), target_pos], layer, 0.15)

    # Ground zones on all signal surfaces and an uninterrupted inner GND plane.
    # F/B zones have a copper-free notch below the ESP32 PCB antenna.
    def add_zone(layer, polygon, clearance=0.2):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(nets["GND"])
        z.SetLocalClearance(mm(clearance))
        z.SetMinThickness(mm(0.15))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        outline = z.Outline()
        outline.NewOutline()
        for x, y in polygon:
            outline.Append(mm(x), mm(y))
        board.Add(z)
        return z

    boundary = [(97.3, 48), (99, 46.3), (135, 46.3), (136.7, 48), (136.7, 128), (135, 129.7), (99, 129.7), (97.3, 128)]
    notched = [(97.3, 48), (99, 46.3), (135, 46.3), (136.7, 48), (136.7, 101.5), (128.3, 101.5), (128.3, 116.5), (136.7, 116.5), (136.7, 128), (135, 129.7), (99, 129.7), (97.3, 128)]
    add_zone(pcbnew.F_Cu, notched)
    add_zone(pcbnew.B_Cu, notched)
    add_zone(pcbnew.In1_Cu, boundary, 0.15)

    # Ground stitching vias, omitted from the ESP antenna keepout.
    stitch_points = [(98.5, y) for y in [52, 58, 64, 70, 76, 82, 88, 94, 100, 106, 112, 126]]
    stitch_points += [(135.5, y) for y in [52, 58, 64, 70, 76, 82, 88, 94, 100, 118, 126]]
    stitch_points += [(x, 48) for x in [103, 108, 113, 118, 123, 128, 133]]
    for x, y in stitch_points:
        via("GND", x, y, 0.60, 0.30)

    # A sparse interior stitching grid keeps the split top/bottom pours tied to
    # the uninterrupted inner ground plane after the autorouter adds signals.
    def stitch_site_is_clear(x, y):
        if x >= 128 and 100.5 < y < 117.0:  # ESP32 antenna keepout
            return False
        px, py = mm(x), mm(y)
        margin = mm(0.5)
        for fp in board.GetFootprints():
            for pad in fp.Pads():
                box = pad.GetBoundingBox()
                if box.GetLeft() - margin <= px <= box.GetRight() + margin and box.GetTop() - margin <= py <= box.GetBottom() + margin:
                    return False
        for item in board.GetTracks():
            box = item.GetBoundingBox()
            if box.GetLeft() - margin <= px <= box.GetRight() + margin and box.GetTop() - margin <= py <= box.GetBottom() + margin:
                return False
        return True

    for xi in range(1000, 1351, 50):
        for yi in range(500, 1276, 50):
            x, y = xi / 10.0, yi / 10.0
            if stitch_site_is_clear(x, y):
                via("GND", x, y, 0.60, 0.30)

    # Board outline, 40 x 84 mm with chamfered corners.
    outline_points = [(99, 46), (135, 46), (137, 48), (137, 128), (135, 130), (99, 130), (97, 128), (97, 48), (99, 46)]
    for a, b in zip(outline_points, outline_points[1:]):
        shape = pcbnew.PCB_SHAPE(board)
        shape.SetShape(pcbnew.SHAPE_T_SEGMENT)
        shape.SetLayer(pcbnew.Edge_Cuts)
        shape.SetStart(pt(*a)); shape.SetEnd(pt(*b)); shape.SetWidth(mm(0.1))
        board.Add(shape)

    def text_item(text, x, y, layer, size=1.0, thickness=0.16, rot=0):
        item = pcbnew.PCB_TEXT(board)
        item.SetText(text); item.SetPosition(pt(x, y)); item.SetLayer(layer)
        item.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        item.SetTextThickness(mm(thickness)); item.SetTextAngle(angle(rot))
        if layer in (pcbnew.B_SilkS, pcbnew.B_Fab):
            item.SetMirrored(True)
        board.Add(item)

    text_item("SMS FWD", 102.5, 126.5, pcbnew.F_SilkS, 1.0, 0.16)
    text_item("ESP32-C3 + ML307A REV 1.0", 117, 123.8, pcbnew.F_SilkS, 0.8, 0.13)
    text_item("LTE", 110, 47, pcbnew.F_SilkS, 0.8)
    text_item("GNSS", 99, 82.5, pcbnew.F_SilkS, 0.8, 0.13, 90)
    text_item("5V 2A", 98.5, 108, pcbnew.F_SilkS, 0.8, 0.13, 90)
    text_item("OPEN HW", 102.5, 128.5, pcbnew.B_SilkS, 0.8, 0.13)
    text_item("github.com/sukiyra/sms-forwarder-board", 135.5, 75, pcbnew.B_SilkS, 0.8, 0.13, 90)
    text_item("RESET", 105, 122.5, pcbnew.F_SilkS, 0.8, 0.13)
    text_item("BOOT", 100.5, 122.5, pcbnew.F_SilkS, 0.8, 0.13)
    text_item("WIFI ANTENNA KEEP CLEAR", 134.5, 109, pcbnew.B_SilkS, 0.8, 0.13, 90)

    # The manual route calls above document preferred topology and critical-line
    # intent.  Remove them before DSN export so Freerouting starts from the
    # complete placement/netlist instead of inheriting provisional geometry.
    for item in list(board.GetTracks()):
        if item.GetNetname() == "GND":
            continue
        board.Remove(item)
    board_file = PROJECT / "sms_forwarder_ml307a.kicad_pcb"
    pcbnew.SaveBoard(str(board_file), board)
    if os.environ.get("SMS_HW_SKIP_DSN") != "1":
        dsn_path = PROJECT / "sms_forwarder_ml307a.dsn"
        # ExportSpecctraDSN may otherwise wait on an overwrite dialog in headless runs.
        if dsn_path.exists():
            dsn_path.unlink()
        pcbnew.ExportSpecctraDSN(board, str(dsn_path))
        contents = dsn_path.read_text(encoding="utf-8")
        first_line, separator, remainder = contents.partition("\n")
        first_line = '(pcb "sms_forwarder_ml307a.dsn"'
        dsn_path.write_text(first_line + separator + remainder, encoding="utf-8")
    return board_file


def generate_project_file():
    PROJECT.mkdir(parents=True, exist_ok=True)
    project = {
        "board": {},
        "boards": [],
        "cvpcb": {},
        "erc": {},
        "libraries": {},
        "meta": {"filename": "sms_forwarder_ml307a.kicad_pro", "version": 1},
        "net_settings": {"classes": [{"name": "Default", "clearance": 0.15, "track_width": 0.25, "via_diameter": 0.65, "via_drill": 0.3}]},
        "pcbnew": {},
        "schematic": {},
        "text_variables": {"PROJECT": "SMS Forwarder Board", "REVISION": "1.0"},
    }
    (PROJECT / "sms_forwarder_ml307a.kicad_pro").write_text(json.dumps(project, indent=2), encoding="utf-8")
    (PROJECT / "fp-lib-table").write_text('(fp_lib_table\n  (lib (name "SMS_Forwarder")(type "KiCad")(uri "${KIPRJMOD}/../libs/SMS_Forwarder.pretty")(options "")(descr ""))\n)\n', encoding="utf-8")
    (PROJECT / "sym-lib-table").write_text('(sym_lib_table\n  (lib (name "SMS_Forwarder")(type "KiCad")(uri "${KIPRJMOD}/../libs/SMS_Forwarder.kicad_sym")(options "")(descr ""))\n  (lib (name "SMS_Board")(type "KiCad")(uri "${KIPRJMOD}/../libs/SMS_Board.kicad_sym")(options "")(descr "Generated schematic symbols"))\n)\n', encoding="utf-8")


def write_bom_csv():
    out = HW / "manufacturing" / "bom"
    out.mkdir(parents=True, exist_ok=True)
    fields = ["Comment", "Designator", "Footprint", "LCSC Part #", "Quantity", "Manufacturer", "Manufacturer Part #", "Assembly", "Notes"]
    def write(path: Path, parts: list[Part]) -> None:
        with path.open("w", newline="", encoding="utf-8-sig") as f:
            w = csv.DictWriter(f, fieldnames=fields)
            w.writeheader()
            for p in parts:
                w.writerow({"Comment": p.value, "Designator": p.refs, "Footprint": p.package, "LCSC Part #": p.lcsc, "Quantity": p.qty, "Manufacturer": p.manufacturer, "Manufacturer Part #": p.mpn, "Assembly": p.assembly, "Notes": p.notes})

    write(out / "BOM_Full.csv", BOM)
    write(out / "BOM_JLCPCB.csv", [part for part in BOM if part.assembly == "SMT"])


def run_cli(*args, check=True):
    return subprocess.run([str(KICAD / "kicad-cli.exe"), *map(str, args)], cwd=ROOT, check=check, text=True, capture_output=True)


def export_outputs(board_file: Path):
    out = HW / "manufacturing"
    gerber = out / "gerber"
    drill = out / "drill"
    pnp = out / "pick-and-place"
    drawings = HW / "drawings"
    for d in [gerber, drill, pnp]:
        if d.exists():
            shutil.rmtree(d)
    for d in [gerber, drill, pnp, drawings]:
        d.mkdir(parents=True, exist_ok=True)
    run_cli("pcb", "drc", "--refill-zones", "--save-board", "--output", out / "DRC-report.txt", board_file, check=False)
    run_cli("pcb", "export", "gerbers", "--output", str(gerber) + os.sep, "--layers", "F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts", board_file)
    run_cli("pcb", "export", "drill", "--output", str(drill) + os.sep, "--format", "excellon", board_file)
    raw_cpl = pnp / "CPL_KICAD.csv"
    run_cli("pcb", "export", "pos", "--output", raw_cpl, "--format", "csv", "--units", "mm", "--side", "both", "--smd-only", "--exclude-dnp", board_file)
    with raw_cpl.open(newline="", encoding="utf-8-sig") as source, (pnp / "CPL_JLCPCB.csv").open("w", newline="", encoding="utf-8-sig") as target:
        reader = csv.DictReader(source)
        writer = csv.DictWriter(target, fieldnames=["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        writer.writeheader()
        for row in reader:
            writer.writerow({
                "Designator": row["Ref"],
                "Mid X": f'{float(row["PosX"]):.4f}mm',
                "Mid Y": f'{float(row["PosY"]):.4f}mm',
                "Layer": row["Side"].title(),
                "Rotation": f'{float(row["Rot"]):.2f}',
            })
    run_cli("pcb", "export", "pdf", "--output", drawings / "pcb-layout.pdf", "--layers", "F.Cu,F.Silkscreen,Edge.Cuts", "--scale", "2", board_file)
    run_cli("pcb", "render", "--output", drawings / "pcb-top.png", "--width", "1800", "--height", "1800", "--side", "top", "--quality", "high", "--background", "opaque", board_file, check=False)
    run_cli("pcb", "render", "--output", drawings / "pcb-bottom.png", "--width", "1800", "--height", "1800", "--side", "bottom", "--quality", "high", "--background", "opaque", board_file, check=False)
    archive = out / "SMS-Forwarder-ML307A-Gerber.zip"
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for source in sorted([*gerber.glob("*"), *drill.glob("*")]):
            if source.is_file():
                bundle.write(source, source.name)


def main():
    generate_project_file()
    board_file = generate_board()
    write_bom_csv()
    # Manufacturing exports are created only after autorouting and final DRC.
    print(board_file)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback
        error_file = ROOT / ".build" / "hardware-generator-error.txt"
        error_file.parent.mkdir(parents=True, exist_ok=True)
        error_file.write_text(traceback.format_exc(), encoding="utf-8")
        raise

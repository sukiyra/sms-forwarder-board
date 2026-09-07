#!/usr/bin/env python3
"""Export DRC, Gerber, drill, CPL and PCB drawing files from the routed board."""

from pathlib import Path

from generate_hardware import export_outputs


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "hardware" / "sms_forwarder_ml307a" / "sms_forwarder_ml307a.kicad_pcb"


if __name__ == "__main__":
    export_outputs(BOARD)

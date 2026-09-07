#!/usr/bin/env python3
"""Import Freerouting's Specctra session and refill all copper zones."""

from pathlib import Path

import pcbnew


ROOT = Path(__file__).resolve().parents[2]
BOARD_PATH = ROOT / "hardware" / "sms_forwarder_ml307a" / "sms_forwarder_ml307a.kicad_pcb"
SESSION_PATH = ROOT / "hardware" / "sms_forwarder_ml307a" / "sms_forwarder_ml307a.ses"


def main() -> None:
    board = pcbnew.LoadBoard(str(BOARD_PATH))
    if not pcbnew.ImportSpecctraSES(board, str(SESSION_PATH)):
        raise RuntimeError(f"Unable to import {SESSION_PATH}")
    for zone in board.Zones():
        zone.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(str(BOARD_PATH), board)


if __name__ == "__main__":
    main()

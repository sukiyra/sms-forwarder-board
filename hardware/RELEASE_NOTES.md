# Hardware Rev 1.0.0

Rev 1.0.0 is the first published engineering-sample hardware design for the SMS Forwarder Board.

## Included

- Editable KiCad 10 schematic, PCB and project libraries.
- 40 × 84 mm, four-layer ESP32-C3-MINI-1 + ML307A board.
- USB-A power/native USB, Nano SIM, UART level translation and modem power switching.
- External LTE MAIN and optional GNSS U.FL connectors with RF matching footprints.
- JLCPCB Gerber, Excellon drill, SMT BOM and dual-side placement data.
- Full BOM workbook, schematic PDF, PCB layout PDF and top/bottom renders.

## Verification state

- KiCad 10.0.6 ERC: 0 errors, 0 warnings.
- KiCad 10.0.6 DRC: 0 violations, 0 unconnected pads, 0 footprint errors.
- BOM/CPL reconciliation: 56 SMT placements in both files.
- Physical first-article validation: pending.

Order five bare PCBs and assemble two first articles before volume production. Follow `ASSEMBLY.md` for staged power, RF, SIM, SMS and 24-hour stability checks.

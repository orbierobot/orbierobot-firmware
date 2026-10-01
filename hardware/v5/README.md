# Orbie V5 electronics (review package)

**Start with `Orbie_V5_Hardware_Review.pdf`** — what was done, how, and the checklist to verify.

| Board | Project | Fab files |
|---|---|---|
| Body controller r0.2 (retrofits the V3 body; outline to be confirmed, see PDF §6) | `orbie_v5_body.kicad_pro` | `out/orbie_v5_body_gerbers.zip`, `_pos.csv`, `_bom_jlc.csv` |
| Belly pogo pad r0.1 | `orbie_v5_pogo.kicad_pro` | `out/orbie_v5_pogo_*` |
| Dock base r0.1 (XIAO ESP32-C3, dead-until-docked pads, IR beacon, feeder/pump driver) | `orbie_v5_dock.kicad_pro` | `out/orbie_v5_dock_*` |
| Feeder module r0.1 | `orbie_v5_feeder.kicad_pro` | `out/orbie_v5_feeder_*` |
| Water module r0.1 | `orbie_v5_water.kicad_pro` | `out/orbie_v5_water_*` |

All five: KiCad 10, 2-layer, autorouted (freerouting), ERC 0 / DRC 0 (`out/*.erc.json`, `out/*.drc.json`).

## Regenerating
The boards are generated from `gen_v5.py` (placement + netlist) using the stock KiCad 10 libraries, then routed by `route_v5.py`:

```
KPY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
$KPY gen_v5.py . body        # or dock | feeder | water | pogo | all
FREEROUTING=/Applications/freerouting.app/Contents/MacOS/freerouting $KPY route_v5.py orbie_v5_body.kicad_pcb --passes 60
```
Edit the generator, not the outputs. Licence: CERN-OHL-S-2.0.

## Status
Hardware only. Not yet written: RP2040 balance firmware (`orbie_motion`), dock ESP32-C3 firmware, head-side ESP-IDF changes (UART to body on GPIO1/2, IR RMT on GPIO3, lidar UART on GPIO4).

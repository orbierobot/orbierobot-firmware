# Orbie V5 — everything the JLCPCB assembly does NOT include

*1 Oct 2026. Quantities are for **2 robots + 1 dock with feeder and water modules**, plus spares where parts are cheap. Prices are rough HK/Shenzhen retail.*

JLCPCB's assembly places only the SMD parts on the **top** side of the body and dock boards (chips, passives, the USB-C receptacle, the slide switch). Four groups are on you:

1. **Hand-fitted parts on the PCBs** — modules, connectors, through-hole parts, the two bottom-side parts.
2. **Off-board electronics** — motors, sensors, speaker, cells, pump, lidar.
3. **Cables** — every JST connector needs a mating housing with crimped wires.
4. **Mechanical bits and consumables** — magnets, screws, feet, paste, flux.

## 1. Hand-fitted on the PCBs (from the design, per board)

| Board | Qty | Part | Note / suggested source |
|---|---|---|---|
| Body | 1 | Seeed XIAO RP2040 | Seeed 102010428, LCSC C2921432. Soldered flat on castellations. |
| Body | 5 | JST PH B6B-PH-K (6-pin header) | motors ×2, ToF ×3 — LCSC C157932 |
| Body | 4 | JST PH B2B-PH-K (2-pin) | bumper, pogo, NTC, speaker — LCSC C131334 |
| Body | 3 | JST PH B3B-PH-K (3-pin) | cliff ×2, servo — LCSC C131335 |
| Body | 1 | JST PH B16B-PH-K (16-pin) | neck harness — LCSC C157921 (check), or 2× B8B if 16 is unavailable |
| Body | 1 | JST XH B2B-XH-A (2-pin, 2.5 mm) | battery — LCSC C144385 |
| Body | 1 | JST ZH B4B-ZR (4-pin, 1.5 mm) | lidar — LCSC C145818 |
| Body | 1 | Everlight IRM-H638T/TR2 38 kHz IR receiver | LCSC C72048 (THT, rear-facing) |
| Body | 2 | Alps SKRKAEE010 side-actuated tact switch | reset / boot — LCSC C2834 or equivalent |
| Body | 1+1 | AH1806-W hall sensor + 100 nF 0603 (bottom side) | LCSC C126719; hand-solder on the back |
| Dock | 1 | Seeed XIAO ESP32-C3 | Seeed 113991054 |
| Dock | 2 | P50-E2 spring pogo pins, Ø0.68 × 16 mm | fit the Ø1 mm PTH pads; AliExpress "P50-E2" |
| Dock | 2 | 5 mm IR LED 940 nm (e.g. TSAL6400) | LCSC C118622 |
| Dock | 1 | JST PH B10B-PH-K (10-pin) | stacking bus — LCSC C157923 |
| Dock | 1 | JST PH B4B-PH-K (4-pin) | optional UART — LCSC C157926 |
| Feeder | 2 | JST PH B10B-PH-K | bus in / out |
| Feeder | 1 | JST PH B2B-PH-K | paddle motor |
| Feeder | 1+1 | 3 mm IR LED 940 nm + 3 mm phototransistor (e.g. PT334-6B) | drop beam across the chute |
| Water | 1 | JST PH B10B-PH-K | bus in |
| Water | 2 | JST PH B2B-PH-K | pump, float switch |
| Pogo | 1 | JST PH B2B-PH-K | to body J4 |

JLCPCB can place all of these JST headers too if you choose their SMT service with "extended parts"; I left them off the assembly list because they are cheap to hand-solder and it keeps the assembly on the economic tier.

## 2. Off-board electronics

| Qty | Part | Spec | Why / source |
|---|---|---|---|
| 4 (+2 spare) | GA12-N20 gearmotor **with magnetic encoder** | 6 V, 300–500 rpm, 6-pin cable (M1 M2 VCC GND A B) | balancing needs encoders; same 12 mm clamp as V3. AliExpress "N20 encoder motor 6V 300RPM". |
| 6 | VL53L5CX breakout | Pololu 3417 or ST SATEL-VL53L5CX | 3 per robot: front, left, right |
| 4 | Downward IR reflective sensor module, **analog** out | TCRT5000 module with A0 pin, or ITR20001 | cliff detection at the front edge |
| 4 | Micro switch KW11 / KW10 with lever | N.O. | bumper, 2 in parallel per robot |
| 2 | Speaker 4 Ω 3 W, **sealed back** | 40 × 20 mm or Ø36 mm with enclosure | the V3 speaker rattles; sealed is REQ-A1 |
| 2 | SG90 / MG90S servo | 5 V | head tilt (reuse V3's) |
| 4 (+2) | 18650 cells, **matched pair**, unprotected | Samsung INR18650-30Q or Molicel P26A | 1S2P per robot; the board has DW01A protection. Buy from one batch, same capacity. |
| 2 | 2× 18650 **parallel** holder or nickel-tabbed 1S2P pack with JST XH lead | 2.5 mm XH plug | cells must be tabbed/joined before first charge (REQ-P3) |
| 2 | 10 kΩ NTC bead, 3435 K, with leads | taped to the cells | charge temperature interlock (REQ-P2) |
| 1–2 | LD19 / LDS06 360° lidar (optional for the rig) | 5 V, UART 230400, ZH 1.5 mm cable supplied | REQ-N3 option B; skip for the first rig |
| 1 | USB-C PD/5 V 3 A power supply + cable | for the dock | |
| 1 | N20 gearmotor (plain, no encoder) | 6 V, 60–100 rpm | feeder paddle drum |
| 1 | Submersible pump 5 V | 2–3 W, 1–2 L/min | water module |
| 1 | Float switch | plastic, N.O. at low level | water module |
| 6 | Neodymium magnet Ø8 × 3 mm | | 1 in each robot belly (hall), 2 under the dock alcove, 2 per stacking deck |

## 3. Cables (one per connector on the boards)

Buy **pre-crimped JST PH 2.0 mm wire sets** (AliExpress: "JST PH 2.0 pre-crimped 150 mm") unless you own a crimper (Engineer PA-09 + PH terminals). Per robot:

| Qty | Cable | Goes to |
|---|---|---|
| 2 | PH 6-pin, 150 mm, housing both ends or bare | motors (most encoder N20s ship with a 6-pin PH or 1.25 mm cable; check and adapt) |
| 3 | PH 6-pin, 100–200 mm | VL53L5CX breakouts (breakouts have 0.1" pins: use PH housing → Dupont female) |
| 2 | PH 3-pin | cliff sensors (modules have 0.1" pins) |
| 1 | PH 3-pin | servo (servo has 0.1" female: PH → Dupont male) |
| 1 | PH 2-pin | speaker |
| 1 | PH 2-pin | bumper switches (daisy-chain two switches) |
| 1 | PH 2-pin, 60 mm | pogo pad board |
| 1 | PH 2-pin | NTC |
| 1 | XH 2-pin, 18 AWG | battery pack |
| 1 | **PH 16-pin, ~150 mm, both ends** | neck harness to the head board. The V3 head board must get a mating 16-pin PH or a wire list; the V3 harness pinout is different. |
| 1 | ZH 4-pin | lidar (comes with the LD19) |

Dock and modules: 3 × PH 10-pin (dock→feeder, feeder→water, spare), 2 × PH 2-pin (paddle motor, pump), 1 × PH 2-pin (float switch), 1 × PH 4-pin (optional).

For the **rig** you can skip crimping: solder wires straight to the header pins on the first two boards.

## 4. Mechanical and consumables

| Qty | Item |
|---|---|
| 8 | M3 × 6 screws for the body board; 8 × M2 × 6 for the dock/module boards |
| 2 | TPU rear rest foot (print) ; 2 × TPU bumper strip for the front |
| 1 | Low-temp solder paste (Sn42Bi58, 138 °C) 50 g — much safer for hand assembly on a hot plate |
| 1 | Hot plate (or reflow plate) 100 × 100 mm, 150–230 °C |
| 1 | Flux pen, IPA, lint-free wipes, fine tweezers, 0.3 mm solder for the hand parts |
| 1 | JLCPCB stencils (already in the quote: body and dock) |
| 2 | 3D-printed bottom shell for the pogo window + foot (later; drill/file the existing shell for the rig) |

## 5. Mounting / wiring notes
- The IMU is on the body board; the board must be screwed down rigidly, no foam tape, or the balance loop sees the mount flex.
- Keep the motor cables away from the I2C runs to the ToF modules; twist each motor pair.
- Fit the hall magnet in the belly directly above where the dock's magnet will sit (12 mm offset from the pogo pads, see dock board).
- Battery: tab the cells, add the NTC between them under Kapton, lead out with XH 2-pin, then connect; the DW01A only protects once the pack is on J5.

# V5 boards vs "Orbie — hardware requirements for indoor autonomy" (26 Sep 2026)

*1 Oct 2026, rev 0.2 of the body board, rev 0.1 of dock / feeder / water boards. Generator: `kicad/gen_v5.py`.*

## How the boards are designed (the process, honestly)

1. **Part choice** comes from my working knowledge of the datasheets of common parts (TI BQ2407x, DRV8833, TCA9534/9517, Maxim MAX98357A, ST LSM6DS3 / VL53L5CX, onsemi LC709203F, Fortune DW01A, Seeed XIAO pinouts). I do not fetch datasheets live while generating; values such as the BQ24074 ILIM/ISET/ITERM resistors, the MT3608 feedback divider and the DRV8833 sense resistor are computed from the datasheet formulas I know. **Each value must be checked against the current datasheet before the order** — that is on the review list below.
2. **Symbols and footprints** come from the stock KiCad 10 libraries, which encode pin names, pin numbers and package dimensions from the manufacturers' drawings. A Python model (`gen_v5.py`) assigns every pin a net by name, so a wrong pin name fails loudly at generation time instead of silently in layout.
3. **Placement** is by hand in the generator (coordinates in mm), checked by KiCad DRC until courtyards, holes and edges are clean.
4. **Routing** is freerouting (autorouter) from a Specctra export, imported back, then GND pours on both layers, then KiCad DRC again. The router works to JLCPCB's 2-layer limits (0.127 mm track/space, 0.3 mm drill).
5. **Fab outputs** are produced by `kicad-cli`: gerbers incl. paste layers (stencil), drill, pick-and-place, and a BOM. LCSC part numbers are *not* filled in; the MPN column is.

What that process is good at: correct netlists, correct packages, no shorts, consistent boards. What it is not: a layout engineer's judgement on the switching loops (MT3608, DRV8833) and the sense-resistor Kelvin connections. Those four spots want five minutes from Sumit before gerbers go out.

## Requirement coverage

| Req | What it asks | Where it is | Status |
|---|---|---|---|
| 2.1 / REQ-C1..C3 | RAM; SLAM off the ESP32; defined MCU↔SoC link, motor timeouts on the MCU | Body board: RP2040 owns motors with its own watchdog; UART to the head; a later Linux SoC talks to the same UART | design rule satisfied; firmware |
| 2.2 / REQ-N3 | VL53L0X unreliable, replace | 3 × VL53L5CX connectors (front, left, right) with LPn lines from the expander for address assignment | **done** (option A) |
| 2.3 / REQ-V1 | camera | head board, not this revision; OV5640 swap recommended | open |
| 2.4 / 2.5 / REQ-A1..A4 | speaker distortion, gain, sample rate, HPF | GAIN strap JP2 (GND 12 dB / open 9 dB / VDD 6 dB / R20 3 dB); amp on the clean 5 V rail; 48 kHz is a firmware change; speaker spec "sealed back 4 Ω 3 W" in BOM | **done** (hardware part) |
| 2.6 / REQ-R3 | thermal | not solved by a board; the Linux SoC decision drives it | open |
| 2.7 / REQ-X1..X6 | reset / boot without disassembly | SW2 RESET and SW3 BOOT, side-actuated, behind rear pinholes; XIAO RST/BOOT carried on harness pins 14/15; 10 s hold = factory reset in firmware; status LED | **done** (hardware part) |
| 2.8 / REQ-R5 | USB brownout / back-feed | D5 Schottky between body 5 V and the head so the XIAO's USB cannot source into a laptop; TVS on VBUS; CC pull-downs | **done** |
| REQ-S1 | topple → motor cut < 200 ms | IMU on the RP2040 bus; RP2040 drives PWM to 50 % (brake) and can drop nSLEEP via expander | firmware on done hardware |
| REQ-S2 | 750 mm drop | mechanical | open |
| REQ-S3 | self-recovery / announce | firmware + speaker | open |
| REQ-S4 | cliff sensors | J10/J11 downward IR, analog into ADS1015 AIN0/1 | **done** |
| REQ-S5 | contact detection | bumper J13 on RP2040 D10 **and** motor current on 0.1 Ω sense into ADS1015 AIN2/3 | **done** (both) |
| REQ-N1 | encoders ≥ 20 cpr | GA12-N20 encoder motors, 6-pin J1/J2, PIO decode | **done** |
| REQ-N2 | IMU to navigation | heading in the UART telemetry | firmware |
| REQ-N4 | lidar in the base | J12 LD19 port: 5 V, TX → head GPIO4 via harness pin 13 | **done** (port only; lidar not in BOM) |
| REQ-N5 / T1 / T5 | dock beacon + contacts, docked = hall AND charge current | IR receiver U13 rear; hall U12; charger CHG/PGOOD on expander | **done** |
| REQ-R1 | I2C integrity | TCA9517A buffer between body bus and the neck harness, 2k2 pull-ups each side, modules on JST | **done** |
| REQ-R4 | 60 min navigation | 1S2P 18650 (V3 pack); budget to be measured | open |
| REQ-P1 | pack protection independent of charger | DW01A + 2 × AO3400A on the pack negative | **done** |
| REQ-P2 | charge temperature interlock | J14 NTC to BQ24074 TS; R9 bypass marked DNP | **done** |
| REQ-P3 | matched cells | procurement note | open |
| REQ-P4 / P5 | shutdown and reserve policy | LC709203F gives `battery_pct`; policy is firmware | firmware |
| REQ-D1 | dock pads dead until robot present | Dock: AO3401A high-side switch, 100 k pull-down, 0.05 Ω shunt read by the C3 ADC, hall + handshake before enable | **done** |
| REQ-D2 | dock must not slide | mechanical (mass / rubber foot) | open |
| REQ-D3 | feeder works without the robot | dock has its own XIAO ESP32-C3 with Wi-Fi and schedule; feeder is a dumb module | **done** |
| REQ-D4 / D5 | food safety, spill path | mechanical; motors are in the modules behind the bus, nothing electrical in the water path | partly, mechanical open |
| REQ-D6 | jam / empty / over-dispense | drop-beam sensor on the feeder board; limits in dock firmware | hardware done |
| REQ-T2..T4 | approach clearance, retry, blocked alcove | firmware + manual | open |

## The five boards

| Board | Size | What | State |
|---|---|---|---|
| `orbie_v5_body` r0.2 | V3 Main PCB outline (**confirm**) | motion MCU, IMU, ADC, charger, protection, gauge, 5 V, 3V3, expander, I2C buffer, amp, 3 ToF, lidar, servo, hall, IR, reset/boot, 16-pin harness | routed, DRC 0 |
| `orbie_v5_pogo` | 28 × 16 | belly pads | routed |
| `orbie_v5_dock` | 70 × 54 | XIAO ESP32-C3, safe pads, IR beacon ×2, hall, DRV8833 for feeder + pump, 10-pin stacking bus, button, LED | routed |
| `orbie_v5_feeder` | 44 × 26 | bus in/out, paddle motor, drop beam, PRESENT_F | routed |
| `orbie_v5_water` | 44 × 26 | bus in, pump, float switch, PRESENT_W | routed |

Stacking bus (JST PH 10-pin on every module, spring contacts on the decks): 5V, GND, FEED_A, FEED_B, PUMP_A, PUMP_B, DROP_SENSE, PRESENT_F, LEVEL_SENSE, PRESENT_W.

## Neck harness J7 (16-pin)

1 GND · 2 +5V_HEAD · 3 +3V3_E · 4 SDA_H · 5 SCL_H · 6 UART_RP_RX (XIAO GPIO1) · 7 UART_RP_TX (GPIO2) · 8 I2S_BCLK (7) · 9 I2S_LRC (8) · 10 I2S_DIN (44) · 11 SERVO_PWM (43) · 12 IR_RX (GPIO3) · 13 LIDAR_RX (GPIO4) · 14 XIAO_RST · 15 XIAO_BOOT · 16 GND

## Review list before ordering

1. **Outline.** The 8 Mar 2026 STEP has no 79.5 × 94.62 board and no 18.6 × 40 bosses; the body holds the 73 × 43 ESP-ROLL board. Ajay to confirm what the body you have mounts, then I re-outline in ten minutes.
2. BQ24074: ILIM 1k1 / ISET 590 / ITERM 1k5 / TMR 10k, and TS behaviour with a 10 k NTC — confirm against the current datasheet rev.
3. DW01A thresholds vs the chosen cells; AO3400A as the protection FETs (5.7 A, fine for a 1S2P pack at 1.5 A charge / ~3 A peak).
4. ADS1015 reading 0–200 mV across the 0.1 Ω sense: use the ±256 mV range.
5. MT3608 feedback 75 k / 22 k → 5.0 V; inductor 4.7 µH ≥ 3 A sat.
6. Sumit to eyeball the autorouted loops around U3, U8 and the two sense resistors.

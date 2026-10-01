# Orbie V5 body controller — build and retrofit notes (rev 0.2)

*30 Sep 2026. STATUS: body board routed (2-layer, 986 tracks), DRC 0 errors, ERC 0 errors; pogo board routed. Gerbers incl. F.Paste (stencil), drill, pick-and-place and BOM in `kicad/v5/out/`. Files: `kicad/v5/`. Generator: `kicad/gen_v5.py`, router/fab script: `kicad/route_v5.py`.*

## What this board is

The V3 body "Main PCB" replaced 1:1 by a balancing controller. Same outline (79.5 × 94.62 mm,
63.74 mm waist), same four M3 holes at 18.6 × 40 mm, so it screws onto the existing posts in the
`Body_Bottom1` shell. The V3 head board with the XIAO ESP32-S3 Sense, display and sensors is
**not** changed; only the neck harness pinout changes (see below).

Two-brain split:

| Board | MCU | Does |
|---|---|---|
| Head (V3, unchanged) | XIAO ESP32-S3 Sense | camera, Wi-Fi/BLE, face, laser, servo PWM, I2S audio out, app API, OTA |
| Body (this board) | XIAO RP2040, soldered flat on castellations | 1 kHz balance loop: LSM6DS3 IMU on its own I2C, two quadrature encoders on PIO, two locked-antiphase PWM channels into a DRV8833, UART to the head |

The head sends velocity setpoints (`drive`, `turn`) over UART; the body replies with pitch, speed,
battery and fault flags. If the head reboots for an OTA the robot keeps standing.

## Retrofit into the V3 body: what fits, what to reprint

| Item | Fits the existing body? | Action |
|---|---|---|
| PCB | yes, same outline and holes | none |
| Motors | **no**, plain N20 has no encoder | GA12-N20 6 V with magnetic encoder, 300–500 rpm. Same 12 mm gearbox, fits the `Motor Clamp`; the encoder disc adds ~9 mm at the back of the motor toward the body centre. Check against the battery support; if it touches, move the pack up 10 mm (it wants to be high anyway). |
| Caster ball + front skids | must go | leave the caster socket empty, remove the skids |
| Rear rest foot | new | small TPU foot on the rear lower shell so it can kneel with motors off. Glue-on for the rig. |
| Pogo pads | new | 28 × 16 mm pad board under the floor; needs two Ø7 mm windows 18 mm apart in the bottom shell. Drill/file for the rig; new print of `Body_Bottom1` later. |
| USB-C | rear edge of the board | slot in the rear shell at board height, or leave for bench charging with the shell off |
| Speaker | front slot as V3 | JST PH2 on the front band |
| ToF | front + both sides | 3 × VL53L5CX modules on 6-pin JST (J15/J16/J17); cliff IR modules on J10/J11 at the front edge facing down |
| Battery | 1S2P 18650 (the March CAD already holds two cells) | XH2 connector through DW01A protection; NTC on the cells to J14; **mount it high** (above the axle) for a balancer |
| Harness | new pinout | 16-pin JST PH, below |

So: **no new body print for the rig**. The only printed part that changes for the product is the bottom shell (pogo windows + foot).

## Neck harness (J7, JST PH 16-pin, rev 0.2)

| Pin | Net | Head side (XIAO ESP32-S3) |
|---|---|---|
| 1 | GND | GND |
| 2 | +5V_HEAD | 5V pin, through D5 so the XIAO USB can never back-feed a laptop |
| 3 | +3V3_E | head sensors if they want the body rail |
| 4 | SDA_H | GPIO5 (through the TCA9517A buffer) |
| 5 | SCL_H | GPIO6 |
| 6 | UART_RP_RX | GPIO1 (TX) — was motor A_FWD |
| 7 | UART_RP_TX | GPIO2 (RX) — was motor A_REV |
| 8 | I2S_BCLK | GPIO7 |
| 9 | I2S_LRC | GPIO8 |
| 10 | I2S_DIN | GPIO44 |
| 11 | SERVO_PWM | GPIO43 |
| 12 | IR_RX | GPIO3 — was motor B_REV |
| 13 | LIDAR_RX | GPIO4 — was motor B_FWD |
| 14 | XIAO_RST | XIAO reset pad (SW2 on the body, rear pinhole) |
| 15 | XIAO_BOOT | XIAO boot pad (SW3; hold 10 s = factory reset in firmware) |
| 16 | GND | GND |

GPIO9 stays the laser.

## Motor drive: locked antiphase

`MOT_L_PWM` goes to DRV8833 AIN1 and, through a 74LVC1G04 inverter, to AIN2. 50 % duty = stop,
above = forward, below = reverse. One RP2040 pin per motor and no dead-zone at zero speed, which is
what a balance loop wants. Cost: some current ripple at zero speed, so run PWM at 20 kHz and expect
the motors to be slightly warm when idle. `JP1` picks VM = VSYS (battery, ~3.7 V) or +5 V boost for
more torque on carpet.

## Firmware to write (not in this repo yet)

- RP2040 (`orbie_motion`): PIO quadrature ×2, LSM6DS3 at 1 kHz, complementary filter, cascaded PID
  (angle inner, speed outer), UART protocol, fall detect, kneel/stand state machine.
- ESP32-S3 `d_wip` changes: remove LEDC motor code; add UART1 on GPIO1/2 to the body; `/motor` becomes a
  setpoint forward; add `/api/motion` telemetry; RMT IR receiver on GPIO3.

## Order list for the rig (2 boards)

| Qty | Part | Note |
|---|---|---|
| 2 | XIAO RP2040 | Seeed 102010428 |
| 4 | GA12-N20 encoder motor 6 V 300–500 rpm | with 6-pin cable |
| 2 | LSM6DS3TR-C | LGA-14 |
| 2 | DRV8833PWPR | HTSSOP-16 |
| 2 | BQ24074RGTR | VQFN-16 |
| 2 | LC709203FQH-01TWG | WDFN-8 |
| 2 | TCA9534PWR | TSSOP-16 |
| 2 | MAX98357AETE+T | QFN-16 |
| 2 | MT3608, AP2112K-3.3, AH1806-W, IRM-H638T | SOT / SC-59 / THT |
| 4 | 74LVC1G04 SOT-23-5 | |
| 2 | AO3400A, AO3401A | SOT-23 |
| 2 | SS14, 4 PMEG2005EJ | |
| 2 | NR4018 4.7 µH inductor | |
| 2 | GCT USB4125 6-pin USB-C | |
| 2 | JST PH B12B, B6B ×2, B3B, B2B ×3, XH B2B | |
| 2 | PCM12 slide switch, 1206 2 A polyfuse | |
| — | 0603 R/C kit, 0805 10 µF, 1210 22 µF ×4 | |
| 2 | ADS1015IDGS, TCA9517ADGKR, DW01A-G, 4× AO3400A extra, SMAJ5.0A ×2 | rev 0.2 additions |
| 6 | VL53L5CX breakout (Pololu / ST SATEL) | 3 per robot |
| 4 | downward IR reflective sensor module, analog out | cliff |
| 2 | 10 k NTC bead, 2 × 0.1 Ω 1206 1 W, 2 × Alps SKRK side tact | rev 0.2 |
| 2 | JST PH B16B (neck), B6B ×3 (ToF), B3B ×2 (cliff), ZH B4B (lidar) | rev 0.2 connectors |
| 1 | Dock set: XIAO ESP32-C3, 2 × 940 nm 5 mm IR LED, 2 × MMBT2222A, 0.05 Ω 2512, P50 pogo pins ×2, DRV8833, TCA9534, PH B10B ×3, PH B2B/B4B | dock + modules |
| 2 | JLCPCB stencil, top side, 0.12 mm | order with the gerbers (body + dock on one panel if possible) |

Full list: `kicad/v5/out/orbie_v5_body_bom_jlc.csv` (add LCSC numbers there if you want JLC to assemble instead).

## Build order in the office

1. Stencil paste, place all SMD on the top side, hot plate at 165 °C (low-temp paste) or 230 °C.
2. Bottom side: hall sensor and the two 0603 by hand.
3. Through-hole: JSTs, USB-C, switch, IR receiver, pin header.
4. Solder the XIAO RP2040 last, flat on its castellations.
5. Power on with no motors: check VSYS, +5V, +3V3_E, +3V3_RP, then I2C scan from the head (expander 0x38, gauge 0x0B) and from the RP2040 (IMU 0x6A).
6. Motors on the bench, then encoders, then the balance loop with the shell open and a hand ready.

# Orbie V5 first-rig shopping list (2 Oct 2026)

Goal: everything needed to bring up **2 body boards** on the bench and fit **1 robot**
(reuse the V3 unit 73B4 for its head board + camera + servo + shell; its speaker is dead).
Prices are approximate HKD, October 2026. Fastest reliable route from Hong Kong:
**Mouser/DigiKey HK** (2–3 days, free shipping over ~HK$400/US$50) for chips and
connectors, **AliExpress** (7–14 days) for motors, modules, cells and tools, and
**Apliu Street, Sham Shui Po** for same-day cells, wire, iron, solder.

## A. Tools (one-time, about HK$1,000)

| # | Item | Why | Pick | ~HKD |
|---|---|---|---|---|
| 1 | Temperature-controlled soldering iron | XIAO castellations, JST headers, IR RX, hall sensor | **Pinecil V2** (USB-C PD, 88 W) — the best cheap iron; or FNIRSI HS-02 / KSGER T12 | 230–300 |
| 2 | USB-C PD 65 W charger + cable | powers the Pinecil (a laptop charger works) | any 65 W PD | 0–120 |
| 3 | Mini hot plate (preheater) | reflow the hand-fitted **LC709203F** (WDFN, no leads) and rework any wrong part | **Miniware MHP30** (HK$550) or a HK$150 "PCB preheater plate 10×10 cm" from AliExpress | 150–550 |
| 4 | Low-temp solder paste Sn42Bi58, 138 °C, syringe | for the hot plate; safe for a beginner | AliExpress "Sn42Bi58 solder paste syringe" | 40 |
| 5 | Solder wire 0.6 mm Sn63/Pb37 or Sn99 lead-free, 100 g | iron work | Kester / Asahi (Apliu St) | 60 |
| 6 | Flux pen or paste (no-clean) | everything | Amtech NC-559 clone / Kingbo | 30 |
| 7 | Desoldering wick 2 mm | fixing bridges on the 16-pin header | | 20 |
| 8 | Fine tweezers (ESD, straight + curved) | | | 30 |
| 9 | IPA 99 % + lint-free wipes + old toothbrush | cleaning flux | Apliu St / pharmacy | 40 |
| 10 | **Digital multimeter** with continuity beep | essential for the rail checks | UNI-T UT33D+ or Aneng AN8008 | 80–150 |
| 11 | Magnifier: USB microscope or 10× loupe | checking pin 1 and bridges | AliExpress "USB microscope 1600x" | 60–120 |
| 12 | Helping hands / PCB vice | | | 50 |
| 13 | Wire stripper + flush cutters | cables | | 60 |
| 14 | Pre-crimped JST PH 2.0 wire sets (see C) | avoids buying a crimper | | — |
| opt | JST PH crimper (Engineer PA-09) + PH terminals | only if you want custom cable lengths | | 300 |

Skip a hot-air station: the hot plate + stencil covers everything on this board.

## B. Parts for the 2 boards (hand-fitted, not placed by JLCPCB)

| Qty | Part | Source | ~HKD |
|---|---|---|---|
| 2 (+1) | Seeed **XIAO RP2040** | Seeed / Mouser 713-102010428 / Apliu St | 60 ea |
| 2 (+1) | **LC709203FQH-01TWG** fuel gauge | Mouser 863-LC709203FQH-01TWG (out of stock at JLC) | 20 ea |
| 10 | JST PH B6B-PH-K header (6-pin, 5/board) | Mouser 306-B6B-PH-K-S | 2 ea |
| 8 | JST PH B2B-PH-K (2-pin, 4/board) | Mouser 306-B2B-PH-K-S | 1.5 ea |
| 6 | JST PH B3B-PH-K (3-pin) | Mouser | 1.5 ea |
| 2 | JST PH B16B-PH-K (16-pin, neck) | Mouser 306-B16B-PH-K-S | 5 ea |
| 2 | JST XH B2B-XH-A (battery) | Mouser | 2 ea |
| 2 | JST ZH B4B-ZR (lidar, optional) | Mouser | 2 ea |
| 2 | Everlight IRM-H638T/TR2 IR receiver | Mouser / LCSC C72048 | 8 ea |
| 2 | PCM12 slide switch (SW1) | Mouser C&K PCM12SMTR | 15 ea |
| 2 | AH1806-W hall sensor + 100 nF 0603 (bottom side) | Mouser / LCSC C126719 | 8 ea |
| 4 | Alps SKRKAEE010 side tact switch (SW2, SW3) | Mouser | 5 ea |

Mouser order total about **HK$350–450**, ships in 2–3 days.

## C. Off-board parts for 1 robot (+ spares)

| Qty | Part | Note | Source | ~HKD |
|---|---|---|---|---|
| 2 (+2) | **GA12-N20 encoder gearmotor 6 V 300 rpm**, 6-pin cable | the V3 motors have no encoders — required for balancing | AliExpress "N20 encoder motor 6V 300RPM" | 35 ea |
| 2 (+2) | 18650 cells, matched, unprotected, 3000 mAh | Molicel P26A / Samsung 30Q; or reuse 73B4's two cells for the first test | Apliu St (ask for "原装 Molicel/Samsung") | 40 ea |
| 1 | 2× 18650 parallel holder with JST XH lead, or nickel strip + Kapton | 1S2P pack | AliExpress | 20 |
| 2 | 10 kΩ NTC bead 3435 K with leads | taped to the cells | AliExpress / Mouser | 5 ea |
| 3 | VL53L5CX breakout | front + left + right ToF | Pololu 3417 (US$) or AliExpress "VL53L5CX module" | 120 ea |
| 2 | TCRT5000 analog IR reflective module (A0 pin) | cliff sensors | AliExpress | 8 ea |
| 2 | KW11 lever microswitch | bumper | Apliu St | 5 ea |
| 1 | Speaker 4 Ω 3 W, 40×20 mm, with sealed enclosure | 73B4's speaker is dead | AliExpress "4ohm 3W speaker cavity 4020" | 25 |
| 1 | SG90 / MG90S servo | head tilt — reuse 73B4's | — | 0 |
| 1 | Head board with **XIAO ESP32-S3 Sense + OV2640 camera** | reuse 73B4's head; buy a spare XIAO ESP32-S3 Sense (HK$120) if you want a bench head | Seeed / Apliu St | 0–120 |
| 1 | USB-C 5 V 3 A supply | bench power | any phone charger | 0 |
| 4 | Ø8×3 mm neodymium magnets | belly + dock later | AliExpress / Apliu St | 10 |
| opt | LD19 lidar | skip for the first rig | AliExpress | 350 |

## D. Cables (pre-crimped, per robot)

AliExpress "JST PH 2.0 pre-crimped cable 150 mm" packs: 2×6-pin both-ends (motors),
3×6-pin one-end (ToF, to Dupont), 3×3-pin, 4×2-pin, 1×16-pin both-ends (neck),
1×XH 2-pin 18 AWG (battery). About **HK$60** total. For the bench you can also solder
wires directly to the header pins.

## E. Totals

| Block | ~HKD |
|---|---|
| Tools | 900–1,300 |
| Board hand parts (Mouser) | 350–450 |
| Off-board, 1 robot + spares | 700–900 |
| Cables | 60 |
| JLCPCB boards (paid separately) | 1,450 + 250 shipping |
| **Everything** | **about 3,700–4,400** |

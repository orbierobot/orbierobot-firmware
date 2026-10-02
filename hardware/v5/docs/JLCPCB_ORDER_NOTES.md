# JLCPCB order notes — body board r0.2 (2 Oct 2026)

Order prepared in the JLCPCB account (cart, NOT paid): `orbie_v5_body_gerbers_Y2`,
5 × PCB (green, 1.6 mm, HASL) **$4.00** + Standard PCBA top side, 5 pcs, 39 line
items placed, stencil included **$180.81** = **$184.81** before shipping.
Lead time: PCB 24 h, assembly 3–4 days.

## Part substitutions made in the JLCPCB BOM match (apply to the KiCad BOM too)

| Ref | Designed | Placed by JLC | LCSC | Why |
|---|---|---|---|---|
| D1, D2 | PMEG2005EJ SOD-323F | RB551V-30 (30 V 2 A SOD-323) | C8529 | PMEG not stocked |
| L1 | 4u7 NR-4018 | ANR4018T4R7M | C7427102 | exact size match |
| U3 | DRV8833PWP | HT8833ARSZ (pin-compatible) | C2928790 | DRV8833 shortfall |
| U14 | DW01A-G | DW01A (SOT-23-6) | C351410 | DW01A-G shortfall |
| D6 | SMAJ5.0A | SMAJ5.0A | C10758 | |
| F1 | 2 A 1206 | SMD1206P200TF (2 A polyfuse) | C20988 | |
| J3 | USB4125-GF-A | USB4125-GF-A-0190 | C5246813 | |
| Q3, Q4 | AO3400A | AO3400A | C20917 | |

## NOT placed by JLCPCB (hand-solder)

| Ref | Part | Note |
|---|---|---|
| U7 | LC709203FQH-01TWG fuel gauge, WDFN-8 3×2 | **every LC709203F variant is out of stock at JLC**. Buy from Mouser/DigiKey/LCSC-global (or an Adafruit LC709203F breakout for the rig) and reflow with the stencil + hot plate. Battery % needs this chip; charger (BQ24074) and protection (DW01A) are placed. |
| SW1 | PCM12 slide switch | THT, hand-solder |
| U4, U5 | 74LVC1G04 SOT-23-5 | placed, but JLC has **no 3D model** for C53185133, so orientation could not be visually verified — check pin 1 against the silk triangle when boards arrive |
| all J*, U1 XIAO, U13 IR RX, Q5 hall | see V5_OFFBOARD_BOM.md | hand-fitted |

## Rotation corrections applied in the JLCPCB placement viewer

JLCPCB's part library is rotated relative to KiCad. Corrections applied so the
viewer's pin-1 dot matches the KiCad pad 1 (top-left at 0°):

| Package | Parts | Correction (clockwise, in viewer) |
|---|---|---|
| SOT-23 / SOT-23-5 | Q1 Q2 Q3 Q4 U9 | +180° |
| SOT-23-6 | U8 U14 | +90° |
| TSSOP / MSOP / HTSSOP | U3 U10 U15 U16 | +90° |
| QFN-16 (MAX98357A) | U11 | +90° |
| VQFN (BQ24074), LGA (LSM6DS3) | U6 U2 | 0 (already correct) |
| Diodes D1–D6 (cathode = pin 1 = "−" in viewer) | | 0 (verified D3 D5 D6; D1 D2 D4 by marker) |

For the **next** order add these offsets in `route_v5.py` when writing the CPL
(per-footprint rotation table) so no manual fixing is needed.

## Still to do before paying
- Decide shipping (DHL ~US$25–35 to HK) and pay in the JLCPCB cart.
- Order the hand-solder parts (above) and the off-board parts (V5_OFFBOARD_BOM.md).

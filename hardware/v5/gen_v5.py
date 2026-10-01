#!/usr/bin/env python3
"""
Orbie V5 boards — generated from one data model with the stock KiCad libraries.

  orbie_v5_body : body controller, retrofit outline of the V3 "Main PCB" drawing
                  (79.5 x 94.62, waist 63.74, 4x M3 at 18.6 x 40).  ** Outline to be
                  confirmed by Ajay: the 8 Mar 2026 STEP has no such board. **
  orbie_v5_pogo : belly pad board (2 x D6 pads, 18 mm pitch)
  orbie_v5_dock : dock base controller (XIAO ESP32-C3, dead-until-docked pads,
                  IR beacon, hall, feeder/pump driver, stacking bus)

Rev 0.2 (1 Oct 2026) applies "Orbie — hardware requirements for indoor autonomy"
(26 Sep 2026): cliff sensors, bumper, motor current sense, 3x VL53L5CX, I2C buffer,
amp gain strap, pack protection + NTC, reset/boot buttons, lidar port, USB TVS,
dead dock pads.

Run with KiCad's python:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 gen_v5.py <out_dir> [body|pogo|dock|all]
"""
import os, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_kicad as G
from gen_kicad import Part, C, R, two_pin, resolve_by_name, write_schematic, write_project, write_pcb, write_bom

G.LAYERS = 2   # hand-assembled prototype: 2-layer

# ------------------------------------------------------------ V3 Main PCB outline (mm, +Y = robot front)
def v3_outline():
    W2, WAIST2, YB, YT, APEX = 39.75, 31.87, 31.72, 45.77, 47.31
    def arc(x0, y0, x1, y1, ya, n=14):
        h = ya - y0; c = x1
        Rr = (c * c + h * h) / (2 * h); cy = ya - Rr
        return [(x0 + (x1 - x0) * k / n, cy + math.sqrt(max(Rr * Rr - (x0 + (x1 - x0) * k / n) ** 2, 0))) for k in range(n + 1)]
    top = arc(-W2, YT, W2, YT, APEX)
    pts = list(top) + [(W2, YB), (WAIST2, YB), (WAIST2, -YB), (W2, -YB)]
    pts += [(x, -y) for (x, y) in reversed(top)]
    pts += [(-W2, -YB), (-WAIST2, -YB), (-WAIST2, YB), (-W2, YB)]
    return pts

HOLES = [(-9.3, 20), (9.3, 20), (-9.3, -20), (9.3, -20)]
HS_C = "Capacitor_SMD:C_0603_1608Metric_Pad1.08x0.95mm_HandSolder"
HS_C8 = "Capacitor_SMD:C_0805_2012Metric_Pad1.18x1.45mm_HandSolder"
HS_R = "Resistor_SMD:R_0603_1608Metric_Pad0.98x0.95mm_HandSolder"
PH = "Connector_JST:JST_PH_B%dB-PH-K_1x%02d_P2.00mm_Vertical"
XIAO_FP = "orbie_v5:XIAO_Castellated"

def ph(n): return PH % (n, n)

def xiao(refA, refB, valA, valB, left, right, pos, pcb, descA, descB):
    """Seeed XIAO as two 7-pin rows on one castellated footprint. left/right = {1..7: net}."""
    pads = {str(k): v for k, v in left.items()}
    pads.update({str(int(k) + 7): v for k, v in right.items()})
    a = Part(refA, "Connector", "Conn_01x07_Pin", valA, XIAO_FP, {str(k): v for k, v in left.items()}, pos, 0, pcb, desc=descA, pad_nets=pads)
    b = Part(refB, "Connector", "Conn_01x07_Pin", valB, XIAO_FP, {str(k): v for k, v in right.items()}, (pos[0] + 30, pos[1]), 0, None, desc=descB)
    return a, b

# ============================================================ BODY BOARD
def body_board(out):
    name = "orbie_v5_body"
    P, T = [], []
    # ---------------- motion MCU
    T.append((20, 18, "U1  XIAO RP2040 — 1 kHz balance loop. D0-D3 encoders (PIO), D4/D5 I2C_M (IMU + ADS1015), D6/D7 UART to head, D8/D9 locked-antiphase PWM, D10 bumper", 2.0))
    a, b = xiao("U1A", "U1B", "XIAO_RP2040 D0-D6", "XIAO_RP2040 D7-D10,3V3,GND,5V",
                {1: "ENC_L_A", 2: "ENC_L_B", 3: "ENC_R_A", 4: "ENC_R_B", 5: "SDA_M", 6: "SCL_M", 7: "UART_RP_TX"},
                {1: "UART_RP_RX", 2: "MOT_L_PWM", 3: "MOT_R_PWM", 4: "BUMPER", 5: "+3V3_RP", 6: "GND", 7: "+5V"},
                (40, 40), (0, 4, 0), "XIAO RP2040 left row D0..D6", "XIAO RP2040 right row (pins 8-14)")
    P += [a, b]
    P.append(C("C1", "10u", "+5V", "GND", (100, 34), (12, 10, 0), fp=HS_C8))
    P.append(Part("J13", "Connector", "Conn_01x02_Pin", "BUMPER switches (N.O., parallel)", ph(2), {"1": "BUMPER", "2": "GND"}, (120, 40), 0, (-30, 34, 0), desc="REQ-S5 contact detect"))
    # ---------------- IMU + ADC on the RP2040 bus
    T.append((160, 18, "U2 LSM6DS3 IMU on the axle line (REQ-S1/N2)   U16 ADS1015: AIN0/1 cliff IR (REQ-S4), AIN2/3 motor current via 0.1R on AISEN/BISEN (REQ-S5)", 2.0))
    imu = {"SDO/SA0": "GND", "SDX": None, "SCX": None, "INT1": "IMU_INT1", "VDDIO": "+3V3_RP", "GND": "GND", "VDD": "+3V3_RP",
           "INT2": None, "NC": None, "CS": "+3V3_RP", "SCL": "SCL_M", "SDA": "SDA_M"}
    P.append(Part("U2", "Sensor_Motion", "LSM6DS3", "LSM6DS3TR-C", "Package_LGA:LGA-14_3x2.5mm_P0.5mm_LayoutBorder3x4y", {}, (190, 45), 0, (0, -9.5, 0), desc="6-axis IMU @0x6A"))
    P.append(C("C2", "100n", "+3V3_RP", "GND", (225, 34), (5, -9.5, 0), fp=HS_C))
    P.append(C("C3", "100n", "+3V3_RP", "GND", (225, 44), (-5, -9.5, 0), fp=HS_C))
    P.append(R("R1", "2k2", "+3V3_RP", "SDA_M", (250, 34), (-6, -12.5, 0), fp=HS_R))
    P.append(R("R2", "2k2", "+3V3_RP", "SCL_M", (250, 44), (-6, -15, 0), fp=HS_R))
    adc = {"VDD": "+3V3_RP", "GND": "GND", "SCL": "SCL_M", "SDA": "SDA_M", "ADDR": "GND", "ALERT/RDY": None,
           "AIN0": "CLIFF_L", "AIN1": "CLIFF_R", "AIN2": "ISEN_A", "AIN3": "ISEN_B"}
    P.append(Part("U16", "Analog_ADC", "ADS1015IDGS", "ADS1015IDGS", "Package_SO:TSSOP-10_3x3mm_P0.5mm", {}, (290, 45), 0, (0, 26, 0), desc="4-ch 12-bit I2C ADC @0x48"))
    P.append(C("C19", "100n", "+3V3_RP", "GND", (325, 34), (6, 26, 0), fp=HS_C))
    for ref, side, x in (("J10", "L", 13), ("J11", "R", 22)):
        P.append(Part(ref, "Connector", "Conn_01x03_Pin", f"CLIFF {side} IR (analog, down)", ph(3), {"1": "+3V3_RP", "2": "GND", "3": f"CLIFF_{side}"}, (350 + (0 if side == "L" else 30), 45), 0, (x, 42, 0), desc="downward IR reflective sensor, front edge"))
    # ---------------- motors
    T.append((20, 95, "U3 DRV8833 locked-antiphase (IN1 = PWM, IN2 = NOT PWM via U4/U5). nSLEEP from expander, default LOW (pull-down). 0.1R sense -> chopping limit 2 A + ADC current sense", 2.0))
    drv = {"VM": "VM", "GND": "GND", "VCP": "DRV_VCP", "VINT": "DRV_VINT", "AIN1": "MOT_L_PWM", "AIN2": "MOT_L_PWM_N",
           "BIN1": "MOT_R_PWM", "BIN2": "MOT_R_PWM_N", "AOUT1": "MOTA_1", "AOUT2": "MOTA_2", "BOUT1": "MOTB_1", "BOUT2": "MOTB_2",
           "AISEN": "ISEN_A", "BISEN": "ISEN_B", "~{SLEEP}": "EXP_P5_DRV_nSLEEP", "~{FAULT}": "DRV_nFAULT"}
    P.append(Part("U3", "Driver_Motor", "DRV8833PWP", "DRV8833PWP", "Package_SO:HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3.4x5mm", {}, (60, 130), 0, (0, -19, 0), desc="dual H-bridge"))
    P.append(C("C4", "10u", "VM", "GND", (100, 115), (0, -13.5, 0), fp=HS_C8))
    P.append(C("C5", "10n", "DRV_VCP", "VM", (100, 125), (6, -12.5, 0), fp=HS_C))
    P.append(C("C6", "2u2", "DRV_VINT", "GND", (100, 135), (6, -15, 0), fp=HS_C))
    P.append(R("R3", "10k", "+3V3_E", "DRV_nFAULT", (100, 145), (14, -24, 0), fp=HS_R))
    P.append(R("R25", "0R1 1W", "ISEN_A", "GND", (130, 150), (-6, -29, 0), fp="Resistor_SMD:R_1206_3216Metric"))
    P.append(R("R26", "0R1 1W", "ISEN_B", "GND", (160, 150), (6, -29, 0), fp="Resistor_SMD:R_1206_3216Metric"))
    P.append(R("R27", "100k", "EXP_P5_DRV_nSLEEP", "GND", (130, 160), (-15, -19, 0), fp=HS_R))
    P.append(Part("U4", "74xGxx", "74LVC1G04", "74LVC1G04", "Package_TO_SOT_SMD:SOT-23-5_HandSoldering", {"2": "MOT_L_PWM", "3": "GND", "4": "MOT_L_PWM_N", "5": "+3V3_RP"}, (130, 120), 0, (-3, -25, 0), desc="inverter L"))
    P.append(Part("U5", "74xGxx", "74LVC1G04", "74LVC1G04", "Package_TO_SOT_SMD:SOT-23-5_HandSoldering", {"2": "MOT_R_PWM", "3": "GND", "4": "MOT_R_PWM_N", "5": "+3V3_RP"}, (130, 140), 0, (3, -25, 0), desc="inverter R"))
    P.append(Part("JP1", "Jumper", "SolderJumper_3_Open", "VM = VSYS | +5V", "Jumper:SolderJumper-3_P1.3mm_Open_RoundedPad1.0x1.5mm", {"1": "VSYS", "2": "VM", "3": "+5V"}, (170, 130), 0, (12, -30, 0), desc="motor rail select"))
    for ref, side, x, m, ea, eb in (("J1", "L", -27, "MOTA", "ENC_L_A", "ENC_L_B"), ("J2", "R", 27, "MOTB", "ENC_R_A", "ENC_R_B")):
        P.append(Part(ref, "Connector", "Conn_01x06_Pin", f"MOTOR {side} N20 encoder", ph(6),
                      {"1": f"{m}_1", "2": f"{m}_2", "3": "+3V3_RP", "4": "GND", "5": ea, "6": eb}, (210 + (0 if side == "L" else 40), 130), 0,
                      (x, -16 if side == "L" else -2, 90 if side == "L" else 270), desc="GA12-N20 with magnetic encoder (REQ-N1)"))
    # ---------------- power in + charger + protection
    T.append((20, 185, "POWER  USB-C 6P (TVS, REQ-R5) or belly POGO -> OR diodes -> BQ24074 1S charger/power path -> VSYS.  Pack: 1S2P 18650 + DW01A/2xAO3400 protection (REQ-P1) + NTC on TS (REQ-P2)", 2.0))
    P.append(Part("J3", "Connector", "USB_C_Receptacle_PowerOnly_6P", "USB-C charge", "Connector_USB:USB_C_Receptacle_GCT_USB4125-xx-x_6P_TopMnt_Horizontal",
                  {"A1": "GND", "B1": "GND", "A4": "VBUS", "B4": "VBUS", "A5": "USB_CC1", "B5": "USB_CC2", "S1": "GND"}, (45, 225), 0, (16, -43.5, 0), desc="USB-C 6P"))
    P.append(R("R4", "5k1", "USB_CC1", "GND", (80, 215), (8, -38, 0), fp=HS_R))
    P.append(R("R5", "5k1", "USB_CC2", "GND", (80, 225), (24, -38, 0), fp=HS_R))
    P.append(two_pin("D6", "Diode", "SMAJ5.0A", "SMAJ5.0A TVS", "Diode_SMD:D_SMA", "GND", "VBUS", (80, 240), 0, (4, -42, 0), desc="USB surge"))
    P.append(Part("J4", "Connector", "Conn_01x02_Pin", "POGO pad board", ph(2), {"1": "DOCK_VIN", "2": "GND"}, (110, 220), 0, (-20, -43, 0), desc="to belly pads"))
    P.append(two_pin("D1", "Diode", "PMEG2005EJ", "PMEG2005EJ", "Diode_SMD:D_SOD-323F", "CHG_IN", "VBUS", (140, 210), 0, (6, -36, 0), desc="OR-ing"))
    P.append(two_pin("D2", "Diode", "PMEG2005EJ", "PMEG2005EJ", "Diode_SMD:D_SOD-323F", "CHG_IN", "DOCK_VIN_F", (140, 225), 0, (0, -36, 0), desc="OR-ing"))
    P.append(two_pin("F1", "Device", "Polyfuse_Small", "2A", "Fuse:Fuse_1206_3216Metric", "DOCK_VIN", "DOCK_VIN_F", (170, 225), 0, (-20, -36, 0)))
    chg = {"IN": "CHG_IN", "OUT": "VSYS", "BAT": "VBAT", "VSS": "GND", "~{CE}": "GND", "EN1": "GND", "EN2": "+3V3_E",
           "ILIM": "CHG_ILIM", "ISET": "CHG_ISET", "ITERM": "CHG_ITERM", "TMR": "CHG_TMR", "TS": "CHG_TS", "~{PGOOD}": "EXP_P1_PGOOD", "~{CHG}": "EXP_P0_CHG"}
    P.append(Part("U6", "Battery_Management", "BQ24074RGT", "BQ24074RGT", "Package_DFN_QFN:VQFN-16-1EP_3x3mm_P0.5mm_EP1.68x1.68mm", {}, (215, 225), 0, (-22, 8, 0), desc="1S charger + power path"))
    P.append(C("C7", "4u7", "CHG_IN", "GND", (255, 205), (-16, 9, 0), fp=HS_C))
    P.append(C("C8", "10u", "VSYS", "GND", (255, 215), (-16, 5, 0), fp=HS_C8))
    P.append(C("C9", "10u", "VBAT", "GND", (255, 225), (-16, 18, 0), fp=HS_C8))
    P.append(R("R6", "1k1 (ILIM 1.5A)", "CHG_ILIM", "GND", (255, 235), (-14, 12, 0), fp=HS_R))
    P.append(R("R7", "590 (ISET 1.5A)", "CHG_ISET", "GND", (255, 245), (-14, 15, 0), fp=HS_R))
    P.append(R("R8", "1k5 (ITERM)", "CHG_ITERM", "GND", (285, 205), (-22, 16, 0), fp=HS_R))
    P.append(R("R9", "10k DNP (TS bypass)", "CHG_TS", "GND", (285, 215), (-22, 26, 0), fp=HS_R))
    P.append(R("R10", "10k (TMR)", "CHG_TMR", "GND", (285, 225), (-22, 29, 0), fp=HS_R))
    P.append(Part("J14", "Connector", "Conn_01x02_Pin", "NTC 10k on cells", ph(2), {"1": "CHG_TS", "2": "GND"}, (285, 245), 0, (-20, 34, 0), desc="REQ-P2 charge temperature interlock"))
    P.append(Part("J5", "Connector", "Conn_01x02_Pin", "BATT 1S2P 18650", "Connector_JST:JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical",
                  {"1": "VBAT_CELLS", "2": "BAT_N"}, (320, 225), 0, (-31, -40, 90), desc="V3 pack (2 cells parallel)"))
    P.append(Part("SW1", "Switch", "SW_SPDT", "POWER", "Button_Switch_SMD:SW_SPDT_PCM12", {"1": None, "2": "VBAT_CELLS", "3": "VBAT"}, (350, 225), 0, (34, -38, 90), desc="main power"))
    # DW01A + 2x AO3400A on the pack negative: B- = BAT_N, P- = GND
    prot = {"OD": "PROT_OD", "OC": "PROT_OC", "CS": "PROT_CS", "VCC": "PROT_VCC", "GND": "BAT_N", "TD": None}
    P.append(Part("U14", "Battery_Management", "DW01A", "DW01A-G", "Package_TO_SOT_SMD:SOT-23-6_Handsoldering", {}, (380, 225), 0, (24, -34, 0), desc="1S protection"))
    P.append(R("R21", "100R", "VBAT_CELLS", "PROT_VCC", (410, 210), (18, -34, 0), fp=HS_R))
    P.append(C("C20", "100n", "PROT_VCC", "BAT_N", (410, 220), (18, -31, 0), fp=HS_C))
    P.append(R("R22", "1k", "PROT_CS", "GND", (410, 230), (22, -31, 0), fp=HS_R))
    P.append(Part("Q3", "Transistor_FET", "AO3400A", "AO3400A (discharge)", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {}, (440, 215), 0, (27, -43, 0), desc="DW01 OD switch"))
    P.append(Part("Q4", "Transistor_FET", "AO3400A", "AO3400A (charge)", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {}, (440, 240), 0, (28, -29.8, 0), desc="DW01 OC switch"))
    q3 = {"G": "PROT_OD", "S": "BAT_N", "D": "PROT_MID"}; q4 = {"G": "PROT_OC", "S": "GND", "D": "PROT_MID"}
    # ---------------- fuel gauge, 5V boost, 3V3
    T.append((20, 275, "U7 LC709203F fuel gauge @0x0B (battery_pct for REQ-P4/P5)   U8 MT3608 5V/2A   U9 AP2112K 3V3_E   D5 isolates the head 5V so the XIAO's USB can never back-feed a laptop (REQ-R5)", 2.0))
    fg = {"V_{DD}": "VBAT", "V_{SS}": "GND", "SDA": "SDA_E", "SCL": "SCL_E", "~{ALARMB}": "FG_ALARM", "T_{SENSE}": None, "T_{SW}": None, "EP": "GND", "TEST": "GND"}
    P.append(Part("U7", "Battery_Management", "LC709203FQH-01TWG", "LC709203FQH-01TWG", "Package_DFN_QFN:WDFN-8-1EP_3x2mm_P0.5mm_EP1.3x1.4mm", {}, (60, 310), 0, (-22, 22, 0), desc="fuel gauge"))
    P.append(C("C10", "1u", "VBAT", "GND", (95, 300), (-16, 22, 0), fp=HS_C))
    boost = {"SW": "BOOST_SW", "GND": "GND", "FB": "BOOST_FB", "EN": "VSYS", "IN": "VSYS", "NC": None}
    P.append(Part("U8", "Regulator_Switching", "MT3608", "MT3608", "Package_TO_SOT_SMD:SOT-23-6", {}, (140, 310), 0, (20, 6, 0), desc="boost"))
    P.append(two_pin("L1", "Device", "L", "4u7 3A", "Inductor_SMD:L_Taiyo-Yuden_NR-40xx", "VSYS", "BOOST_SW", (180, 300), 0, (20, 14, 0)))
    P.append(two_pin("D3", "Diode", "SS14", "SS14", "Diode_SMD:D_SMA", "+5V", "BOOST_SW", (180, 310), 0, (26, 8, 90)))
    P.append(R("R11", "75k", "+5V", "BOOST_FB", (210, 300), (14, 2, 0), fp=HS_R))
    P.append(R("R12", "22k", "BOOST_FB", "GND", (210, 310), (18, 2, 0), fp=HS_R))
    P.append(C("C11", "22u", "+5V", "GND", (240, 300), (22, 27, 0), fp="Capacitor_SMD:C_1210_3225Metric_Pad1.33x2.70mm_HandSolder"))
    P.append(C("C12", "22u", "VSYS", "GND", (240, 310), (20, -4, 0), fp="Capacitor_SMD:C_1210_3225Metric_Pad1.33x2.70mm_HandSolder"))
    ldo = {"VIN": "VSYS", "GND": "GND", "EN": "VSYS", "NC": None, "VOUT": "+3V3_E"}
    P.append(Part("U9", "Regulator_Linear", "AP2112K-3.3", "AP2112K-3.3", "Package_TO_SOT_SMD:SOT-23-5_HandSoldering", {}, (290, 310), 0, (16, 29, 0), desc="3.3V LDO (board rail)"))
    P.append(C("C13", "1u", "VSYS", "GND", (325, 300), (26, 29.5, 0), fp=HS_C))
    P.append(C("C14", "1u", "+3V3_E", "GND", (325, 310), (16, 22, 0), fp=HS_C))
    P.append(two_pin("D5", "Diode", "SS14", "SS14", "Diode_SMD:D_SMA", "+5V_HEAD", "+5V", (360, 305), 0, (-8, 30, 0), desc="head 5V isolation"))
    # ---------------- head I2C bus: buffer, expander, 3x ToF, hall, IR, servo, speaker
    T.append((20, 345, "U15 TCA9517A I2C buffer: body bus (A) <-> neck harness (B) so a failing head sensor cannot hang the body (REQ-R1)   U10 TCA9534 @0x38: CHG, PGOOD, nFAULT, SERVO_PWR, DOCK_DET, DRV_nSLEEP, ToF LPn L/R", 2.0))
    buf = {"VCCA": "+3V3_E", "VCCB": "+3V3_E", "GND": "GND", "SDAA": "SDA_E", "SCLA": "SCL_E", "SDAB": "SDA_H", "SCLB": "SCL_H", "EN": "+3V3_E"}
    P.append(Part("U15", "Logic_LevelTranslator", "TCA9517ADGK", "TCA9517ADGKR", "Package_SO:MSOP-8_3x3mm_P0.65mm", {}, (60, 380), 0, (-20, -10, 0), desc="I2C buffer"))
    P.append(C("C21", "100n", "+3V3_E", "GND", (95, 370), (-14, -10, 0), fp=HS_C))
    P.append(R("R23", "2k2", "+3V3_E", "SDA_H", (95, 380), (-12, -4, 0), fp=HS_R))
    P.append(R("R24", "2k2", "+3V3_E", "SCL_H", (95, 390), (-12, -7, 0), fp=HS_R))
    tca = {"SDA": "SDA_E", "SCL": "SCL_E", "~{INT}": "EXP_INT", "P0": "EXP_P0_CHG", "P1": "EXP_P1_PGOOD", "P2": "DRV_nFAULT", "P3": "EXP_P3_SERVO_PWR_EN",
           "P4": "EXP_P4_DOCK_DET", "P5": "EXP_P5_DRV_nSLEEP", "P6": "TOF_LPN_L", "P7": "TOF_LPN_R", "A0": "GND", "A1": "GND", "A2": "GND", "GND": "GND", "VDD": "+3V3_E"}
    P.append(Part("U10", "Interface_Expansion", "TCA9534", "TCA9534PWR", "Package_SO:TSSOP-16_4.4x5mm_P0.65mm", {}, (140, 380), 0, (-20, -26, 0), desc="I/O expander"))
    P.append(C("C15", "100n", "+3V3_E", "GND", (175, 365), (-29, -24, 0), fp=HS_C))
    P.append(R("R13", "2k2", "+3V3_E", "SDA_E", (205, 365), (-29, -20, 0), fp=HS_R))
    P.append(R("R14", "2k2", "+3V3_E", "SCL_E", (205, 375), (-29, -28, 0), fp=HS_R))
    P.append(R("R15", "10k", "+3V3_E", "EXP_INT", (205, 385), (-13, -28, 0), fp=HS_R))
    P.append(R("R16", "10k", "+3V3_E", "FG_ALARM", (205, 395), (-25, -30, 0), fp=HS_R))
    T.append((230, 345, "J15/J16/J17  3x VL53L5CX 8x8 multizone ToF (front, left, right) on the body bus (REQ-N3 option A). Front LPn tied high; L/R LPn from the expander to assign addresses at boot", 2.0))
    for ref, side, pcbpos, lpn in (("J15", "FRONT", (-2, 42, 0), "+3V3_E"), ("J16", "LEFT", (-29, 14, 90), "TOF_LPN_L"), ("J17", "RIGHT", (29, 14, 90), "TOF_LPN_R")):
        P.append(Part(ref, "Connector", "Conn_01x06_Pin", f"VL53L5CX {side}", ph(6), {"1": "+3V3_E", "2": "GND", "3": "SDA_E", "4": "SCL_E", "5": lpn, "6": None},
                      (250 + {"FRONT": 0, "LEFT": 40, "RIGHT": 80}[side], 380), 0, pcbpos, desc="8x8 ToF module"))
    T.append((230, 400, "AUDIO  MAX98357A on +5V. JP2 GAIN strap: GND=12 dB | open=9 dB | VDD=6 dB, R20 100k->VDD = 3 dB (REQ-A2). Speaker: sealed-back 4R 3W (REQ-A1)", 2.0))
    amp = {"~{SD_MODE}": "+3V3_E", "NC": None, "GAIN_SLOT": "AMP_GAIN", "DIN": "I2S_DIN", "BCLK": "I2S_BCLK", "LRCLK": "I2S_LRC",
           "GND": "GND", "VDD": "+5V", "OUTP": "SPK_P", "OUTN": "SPK_N", "PAD": "GND"}
    P.append(Part("U11", "Audio", "MAX98357A", "MAX98357AETE+T", "Package_DFN_QFN:QFN-16-1EP_3x3mm_P0.5mm_EP1.75x1.75mm", {}, (250, 430), 0, (14, 36, 0), desc="I2S class-D"))
    P.append(C("C16", "10u", "+5V", "GND", (285, 420), (8, 36, 0), fp=HS_C8))
    P.append(C("C17", "100n", "+5V", "GND", (285, 430), (4, 36, 0), fp=HS_C))
    P.append(Part("JP2", "Jumper", "SolderJumper_3_Open", "GAIN: GND | open | VDD", "Jumper:SolderJumper-3_P1.3mm_Open_RoundedPad1.0x1.5mm", {"1": "GND", "2": "AMP_GAIN", "3": "+5V"}, (320, 425), 0, (10, 32, 0), desc="amp gain strap"))
    P.append(R("R20", "100k DNP (3 dB)", "AMP_GAIN", "+5V", (320, 440), (16, 32, 0), fp=HS_R))
    P.append(Part("J6", "Connector", "Conn_01x02_Pin", "SPEAKER sealed 4R", ph(2), {"1": "SPK_P", "2": "SPK_N"}, (350, 430), 0, (36, 40, 90), desc="speaker"))
    # ---------------- harness, lidar, servo, dock sensing, reset/boot
    T.append((20, 470, "J7 NECK HARNESS 16p: GND +5V_HEAD 3V3 SDA_H SCL_H UART I2S SERVO IR_RX LIDAR_RX XIAO_RST XIAO_BOOT.  J12 LD19 lidar in the base (REQ-N4): 5V, TX->GPIO4 of the head XIAO.  SW2/SW3 recessed RESET/BOOT (REQ-X1..X4)", 2.0))
    neck = {"1": "GND", "2": "+5V_HEAD", "3": "+3V3_E", "4": "SDA_H", "5": "SCL_H", "6": "UART_RP_RX", "7": "UART_RP_TX", "8": "I2S_BCLK", "9": "I2S_LRC",
            "10": "I2S_DIN", "11": "SERVO_PWM", "12": "IR_RX", "13": "LIDAR_RX", "14": "XIAO_RST", "15": "XIAO_BOOT", "16": "GND"}
    P.append(Part("J7", "Connector", "Conn_01x16_Pin", "NECK HARNESS 16p", ph(16), neck, (45, 505), 0, (-37, 40, 0), desc="to head board"))
    T.append((60, 530, "head side: 6=GPIO1(TX) 7=GPIO2(RX) 12=GPIO3 13=GPIO4 14/15=XIAO RST/BOOT pads", 1.5))
    P.append(Part("J12", "Connector", "Conn_01x04_Pin", "LIDAR LD19 (5V GND TX PWM)", "Connector_JST:JST_ZH_B4B-ZR_1x04_P1.50mm_Vertical", {"1": "+5V", "2": "GND", "3": "LIDAR_RX", "4": None}, (110, 505), 0, (16, -12, 0), desc="360° lidar, base mounted"))
    P.append(Part("J9", "Connector", "Conn_01x03_Pin", "SERVO head tilt", ph(3), {"1": "SERVO_PWM", "2": "+5V_SERVO", "3": "GND"}, (150, 505), 0, (22, 34, 0), desc="SG90"))
    pfet = {"G": "SERVO_GATE", "S": "+5V", "D": "+5V_SERVO"}; nfet = {"G": "EXP_P3_SERVO_PWR_EN", "S": "GND", "D": "SERVO_GATE"}
    P.append(Part("Q1", "Transistor_FET", "AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {}, (190, 495), 0, (14, -2, 0), desc="servo P-FET"))
    P.append(Part("Q2", "Transistor_FET", "AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {}, (190, 520), 0, (28, -18, 0), desc="level shift"))
    P.append(R("R17", "10k", "+5V", "SERVO_GATE", (225, 490), (28, 3, 0), fp=HS_R))
    P.append(R("R18", "100k", "EXP_P3_SERVO_PWR_EN", "GND", (225, 525), (28, -22, 0), fp=HS_R))
    hall = {"VDD": "+3V3_E", "GND": "GND", "OUTPUT": "EXP_P4_DOCK_DET"}
    P.append(Part("U12", "Sensor_Magnetic", "AH1806-W", "AH1806-W", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {}, (260, 505), 0, (-14, -34, 0), side="B", desc="hall: dock magnet (REQ-T5 with charge current)"))
    ir = {"OUT": "IR_RX", "GND": "GND", "Vs": "+3V3_E"}
    P.append(Part("U13", "Interface_Optical", "TSOP38G36", "IRM-H638T 38kHz (rear)", "OptoDevice:Everlight_IRM-H6xxT", {}, (300, 505), 0, (-6, -34, 180), desc="dock beacon receiver"))
    P.append(C("C18", "100n", "+3V3_E", "GND", (330, 495), (-20, -30, 0), fp=HS_C))
    P.append(Part("D4", "Device", "LED", "STATUS (ember)", "LED_SMD:LED_0603_1608Metric", {"1": "GND", "2": "LED_K"}, (360, 495), 0, (37, -44, 0)))
    P.append(R("R19", "1k", "+3V3_E", "LED_K", (360, 510), (33, -45, 0), fp=HS_R))
    P.append(Part("SW2", "Switch", "SW_Push", "RESET (pinhole)", "Button_Switch_SMD:SW_Push_SPST_NO_Alps_SKRK", {"1": "XIAO_RST", "2": "GND"}, (400, 495), 0, (-10, -44, 0), desc="side-actuated, through rear shell pinhole"))
    P.append(Part("SW3", "Switch", "SW_Push", "BOOT / hold 10 s = factory reset", "Button_Switch_SMD:SW_Push_SPST_NO_Alps_SKRK", {"1": "XIAO_BOOT", "2": "GND"}, (400, 520), 0, (-4, -44, 0), desc="side-actuated, through rear shell pinhole"))
    resolve_by_name(P, {"U2": imu, "U16": adc, "U3": drv, "U6": chg, "U7": fg, "U8": boost, "U9": ldo, "U14": prot, "Q3": q3, "Q4": q4,
                        "U15": buf, "U10": tca, "U11": amp, "Q1": pfet, "Q2": nfet, "U12": hall, "U13": ir})
    texts = [(20, 10, "ORBIE V5 — BODY CONTROLLER r0.2 (applies HW requirements 26 Sep 2026). Retrofit outline of the V3 Main PCB drawing — CONFIRM mounting pattern against the body before fab.", 3.0)] + T
    pf = [("+5V", (420, 40)), ("VBUS", (440, 40)), ("GND", (460, 40)), ("DOCK_VIN", (480, 40)), ("+5V_SERVO", (500, 40)), ("+3V3_RP", (520, 40)),
          ("VBAT_CELLS", (540, 40)), ("DRV_VINT", (560, 40)), ("CHG_IN", (580, 40)), ("VM", (600, 40)), ("BAT_N", (620, 40)), ("+5V_HEAD", (640, 40)), ("PROT_VCC", (660, 40))]
    root = write_schematic(os.path.join(out, name + ".kicad_sch"), name, P, texts, pf, paper="A1", title="Orbie V5 Body Controller", rev="0.2")
    ptexts = [(0, 30, "ORBIE V5 BODY r0.2", 1.2, "F_SilkS"), (0, -46.3, "REAR", 0.8, "F_SilkS"), (0, 46.3, "FRONT", 0.8, "F_SilkS"),
              (0, -6, "IMU ON AXLE", 0.7, "F_SilkS"), (20, -46.3, "CERN-OHL-S-2.0", 0.7, "F_SilkS"), (-10, -47, "RST  BOOT", 0.6, "F_SilkS")]
    write_pcb(os.path.join(out, name + ".kicad_pcb"), P, v3_outline(), HOLES, ptexts, "Orbie V5 Body Controller", hole_fp="MountingHole_3.2mm_M3_Pad")
    write_project(os.path.join(out, name + ".kicad_pro"), name, root)
    return P

# ============================================================ POGO PAD BOARD
def pogo_board(out):
    name = "orbie_v5_pogo"
    P = [Part("PAD1", "Connector", "TestPoint", "POGO+ D6", "orbie_v5:PogoPad_D6mm", {"1": "DOCK_VIN"}, (40, 40), 0, (-7, -2, 0), side="B", desc="belly pad +"),
         Part("PAD2", "Connector", "TestPoint", "POGO- D6", "orbie_v5:PogoPad_D6mm", {"1": "GND"}, (60, 40), 0, (7, -2, 0), side="B", desc="belly pad -"),
         Part("J1", "Connector", "Conn_01x02_Pin", "to J4 body", ph(2), {"1": "DOCK_VIN", "2": "GND"}, (80, 40), 0, (0, 4.5, 0), desc="to body J4")]
    texts = [(20, 10, "ORBIE V5 — BELLY POGO PAD BOARD 28x16 mm. Pads face DOWN through a window in the body floor; 14 mm pitch matches the dock pins.", 2.5)]
    root = write_schematic(os.path.join(out, name + ".kicad_sch"), name, P, texts, [("DOCK_VIN", (100, 40)), ("GND", (120, 40))], paper="A4", title="Orbie V5 Pogo Pad Board")
    write_pcb(os.path.join(out, name + ".kicad_pcb"), P, (28, 16, 2), [(-9.5, 4.5), (9.5, 4.5)], [(0, 6.2, "POGO r0.1", 0.7, "F_SilkS")], "Orbie V5 Pogo Pad Board")
    write_project(os.path.join(out, name + ".kicad_pro"), name, root)
    return P

# ============================================================ DOCK BASE BOARD
def dock_board(out):
    name = "orbie_v5_dock"
    P, T = [], []
    T.append((20, 18, "U1 XIAO ESP32-C3 — dock controller (Wi-Fi to app, schedules feeding without the robot, REQ-D3). D0 hall, D1 pad enable, D2 ISENSE (ADC), D3 drop sensor, D4/D5 bus I2C, D6/D7 UART to bus, D8 IR_L, D9 IR_R, D10 button", 2.0))
    a, b = xiao("U1A", "U1B", "XIAO_ESP32C3 D0-D6", "XIAO_ESP32C3 D7-D10,3V3,GND,5V",
                {1: "HALL_DET", 2: "PAD_EN", 3: "ISENSE", 4: "DROP_SENSE", 5: "SDA_BUS", 6: "SCL_BUS", 7: "BUS_TX"},
                {1: "BUS_RX", 2: "IR_L_PWM", 3: "IR_R_PWM", 4: "BTN", 5: "+3V3", 6: "GND", 7: "+5V"},
                (40, 40), (0, 2, 0), "XIAO ESP32-C3 left row", "XIAO ESP32-C3 right row")
    P += [a, b]
    P.append(C("C1", "10u", "+5V", "GND", (100, 34), (12, 0, 0), fp=HS_C8))
    T.append((20, 90, "POWER IN  USB-C 5V/3A (CC 5k1), TVS, 3A polyfuse.  PADS: AO3401A high-side switch, 100k pull-down, 0.05R shunt + ADC fold-back; pads are DEAD until hall + handshake (REQ-D1)", 2.0))
    P.append(Part("J1", "Connector", "USB_C_Receptacle_PowerOnly_6P", "USB-C 5V in", "Connector_USB:USB_C_Receptacle_GCT_USB4125-xx-x_6P_TopMnt_Horizontal",
                  {"A1": "GND", "B1": "GND", "A4": "VBUS", "B4": "VBUS", "A5": "CC1", "B5": "CC2", "S1": "GND"}, (45, 125), 0, (-20, -24.7, 0), desc="USB-C 6P"))
    P.append(R("R1", "5k1", "CC1", "GND", (80, 115), (-30, -17, 0), fp=HS_R))
    P.append(R("R2", "5k1", "CC2", "GND", (80, 125), (-26, -17, 0), fp=HS_R))
    P.append(two_pin("D1", "Diode", "SMAJ5.0A", "SMAJ5.0A TVS", "Diode_SMD:D_SMA", "GND", "VBUS", (80, 140), 0, (-12, -18, 0)))
    P.append(two_pin("F1", "Device", "Polyfuse_Small", "3A", "Fuse:Fuse_1206_3216Metric", "VBUS", "+5V", (110, 125), 0, (-4, -18, 0)))
    P.append(C("C2", "22u", "+5V", "GND", (140, 125), (4, -18, 0), fp="Capacitor_SMD:C_1210_3225Metric_Pad1.33x2.70mm_HandSolder"))
    pfet = {"G": "PAD_GATE", "S": "+5V", "D": "PAD_V"}; nfet = {"G": "PAD_EN", "S": "GND", "D": "PAD_GATE"}
    P.append(Part("Q1", "Transistor_FET", "AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {}, (180, 115), 0, (12, -14, 0), desc="pad high-side switch"))
    P.append(Part("Q2", "Transistor_FET", "AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {}, (180, 140), 0, (12, -20, 0), desc="level shift"))
    P.append(R("R3", "10k", "+5V", "PAD_GATE", (215, 110), (15, -9, 0), fp=HS_R))
    P.append(R("R4", "100k", "PAD_EN", "GND", (215, 145), (17, -20, 0), fp=HS_R))
    P.append(R("R5", "0R05 1W", "PAD_V", "PAD_OUT", (250, 115), (22, -16, 0), fp="Resistor_SMD:R_2512_6332Metric"))
    P.append(R("R6", "100k (pads dead)", "PAD_OUT", "GND", (250, 130), (28, -4, 0), fp=HS_R))
    P.append(R("R7", "10k", "PAD_V", "ISENSE", (280, 115), (22, -10, 0), fp=HS_R))
    P.append(R("R8", "10k", "ISENSE", "PAD_OUT", (280, 130), (22, -6, 0), fp=HS_R))
    T.append((250, 148, "ISENSE = midpoint of the shunt divider -> C3 ADC reads (PAD_V-PAD_OUT)/2 referenced to PAD_OUT; firmware folds back > 2.5 A and reports shorts", 1.4))
    P.append(Part("TP1", "Connector", "TestPoint", "POGO PIN + (P50 spring)", "TestPoint:TestPoint_THTPad_D2.0mm_Drill1.0mm", {"1": "PAD_OUT"}, (320, 115), 0, (-7, 22, 0), desc="spring pin, 14 mm pitch"))
    P.append(Part("TP2", "Connector", "TestPoint", "POGO PIN - (P50 spring)", "TestPoint:TestPoint_THTPad_D2.0mm_Drill1.0mm", {"1": "GND"}, (320, 135), 0, (7, 22, 0), desc="spring pin"))
    T.append((20, 175, "HOMING  2x 940 nm IR LEDs (L/R lobes) at 38 kHz from the C3 via MMBT2222A, 33R from 5V.  U2 hall AH1806: robot magnet = present.  SW1 button: feed now / hold = pair", 2.0))
    for ref, side, x in (("D2", "L", -20), ("D3", "R", 20)):
        P.append(Part(ref, "Device", "LED", f"IR LED 940nm {side}", "LED_THT:LED_D5.0mm", {"1": f"IR_{side}_K", "2": "+5V"}, (45 + (0 if side == "L" else 90), 205), 0, (x, 20, 0), desc="beacon lobe"))
        P.append(R(f"R{9 if side=='L' else 10}", "33R 1W", f"IR_{side}_K", f"IR_{side}_C", (75 + (0 if side == "L" else 90), 200), (x, 14, 0), fp="Resistor_SMD:R_1206_3216Metric"))
        P.append(Part(f"Q{3 if side=='L' else 4}", "Transistor_BJT", "MMBT2222A", "MMBT2222A", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {"1": f"IR_{side}_BASE", "2": "GND", "3": f"IR_{side}_C"}, (75 + (0 if side == "L" else 90), 215), 0, (x, 9, 0), desc="LED driver"))
        P.append(R(f"R{11 if side=='L' else 12}", "1k", f"IR_{side}_PWM", f"IR_{side}_BASE", (105 + (0 if side == "L" else 90), 215), (x + (6 if side == "R" else 5), 9, 0), fp=HS_R))
    hall = {"VDD": "+3V3", "GND": "GND", "OUTPUT": "HALL_DET"}
    P.append(Part("U2", "Sensor_Magnetic", "AH1806-W", "AH1806-W", "Package_TO_SOT_SMD:SOT-23_Handsoldering", {}, (230, 205), 0, (0, 17, 0), desc="hall: robot present"))
    P.append(C("C3", "100n", "+3V3", "GND", (260, 200), (-5, 17, 0), fp=HS_C))
    P.append(Part("SW1", "Switch", "SW_Push", "FEED / PAIR", "Button_Switch_SMD:SW_Push_1P1T_NO_CK_KMR2", {"1": "BTN", "2": "GND"}, (290, 205), 0, (30, 13, 0), desc="user button"))
    P.append(Part("D4", "Device", "LED", "STATUS", "LED_SMD:LED_0603_1608Metric", {"1": "GND", "2": "LED_K"}, (320, 200), 0, (30, 4, 0)))
    P.append(R("R13", "1k", "+3V3", "LED_K", (320, 215), (30, 8, 0), fp=HS_R))
    T.append((20, 245, "MODULES  U3 DRV8833 drives the feeder paddle motor (A) and the water pump (B) through the stacking bus J2 (10p: 5V GND FEED_A FEED_B PUMP_A PUMP_B DROP PRESENT_F LEVEL PRESENT_W). Modules are dumb; motors live in the modules (REQ-D4)", 2.0))
    drv = {"VM": "+5V", "GND": "GND", "VCP": "DRV_VCP", "VINT": "DRV_VINT", "AIN1": "FEED_IN1", "AIN2": "FEED_IN2", "BIN1": "PUMP_IN1", "BIN2": "PUMP_IN2",
           "AOUT1": "FEED_A", "AOUT2": "FEED_B", "BOUT1": "PUMP_A", "BOUT2": "PUMP_B", "AISEN": "GND", "BISEN": "GND", "~{SLEEP}": "+3V3", "~{FAULT}": None}
    P.append(Part("U3", "Driver_Motor", "DRV8833PWP", "DRV8833PWP", "Package_SO:HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3.4x5mm", {}, (60, 285), 0, (-18, 2, 0), desc="feeder + pump driver"))
    P.append(C("C4", "10u", "+5V", "GND", (100, 270), (-24, 6, 0), fp=HS_C8))
    P.append(C("C5", "10n", "DRV_VCP", "+5V", (100, 280), (-24, 2, 0), fp=HS_C))
    P.append(C("C6", "2u2", "DRV_VINT", "GND", (100, 290), (-24, -2, 0), fp=HS_C))
    # motor inputs from an I2C expander would cost pins; use the bus I2C lines' expander on the C3? keep simple: TCA9534 on the dock bus
    tca = {"SDA": "SDA_BUS", "SCL": "SCL_BUS", "~{INT}": None, "P0": "FEED_IN1", "P1": "FEED_IN2", "P2": "PUMP_IN1", "P3": "PUMP_IN2", "P4": "PRESENT_F",
           "P5": "PRESENT_W", "P6": "LEVEL_SENSE", "P7": None, "A0": "GND", "A1": "GND", "A2": "GND", "GND": "GND", "VDD": "+3V3"}
    P.append(Part("U4", "Interface_Expansion", "TCA9534", "TCA9534PWR", "Package_SO:TSSOP-16_4.4x5mm_P0.65mm", {}, (160, 285), 0, (-18, -8, 0), desc="expander: motor inputs + module present"))
    P.append(C("C7", "100n", "+3V3", "GND", (195, 270), (-24, -8, 0), fp=HS_C))
    P.append(R("R14", "2k2", "+3V3", "SDA_BUS", (225, 270), (-12, -5, 0), fp=HS_R))
    P.append(R("R15", "2k2", "+3V3", "SCL_BUS", (225, 280), (-12, -8, 0), fp=HS_R))
    P.append(Part("J2", "Connector", "Conn_01x10_Pin", "STACK BUS 10p", ph(10), {"1": "+5V", "2": "GND", "3": "FEED_A", "4": "FEED_B", "5": "PUMP_A", "6": "PUMP_B", "7": "DROP_SENSE", "8": "PRESENT_F", "9": "LEVEL_SENSE", "10": "PRESENT_W"},
                  (260, 285), 0, (-30, -6, 90), desc="to feeder / water modules (spring contacts on the deck)"))
    P.append(R("R16", "10k", "+3V3", "DROP_SENSE", (300, 275), (-18, 12, 0), fp=HS_R))
    P.append(R("R17", "10k", "+3V3", "PRESENT_F", (300, 285), (-14, 12, 0), fp=HS_R))
    P.append(R("R18", "10k", "+3V3", "LEVEL_SENSE", (300, 295), (-10, 17, 0), fp=HS_R))
    P.append(R("R19", "10k", "+3V3", "PRESENT_W", (300, 305), (-12, 19.5, 0), fp=HS_R))
    P.append(Part("J3", "Connector", "Conn_01x04_Pin", "BUS UART (opt. smart module)", ph(4), {"1": "+5V", "2": "GND", "3": "BUS_TX", "4": "BUS_RX"}, (340, 285), 0, (30, -18, 90), desc="optional"))
    resolve_by_name(P, {"Q1": pfet, "Q2": nfet, "U2": hall, "U3": drv, "U4": tca})
    texts = [(20, 10, "ORBIE V5 — DOCK BASE BOARD r0.1. Pads dead until a robot is present; feeder works without the robot; 70 x 54 mm, 2-layer.", 3.0)] + T
    pf = [("+5V", (380, 40)), ("+3V3", (400, 40)), ("GND", (420, 40)), ("VBUS", (440, 40)), ("PAD_V", (460, 40)), ("PAD_OUT", (480, 40)), ("DRV_VINT", (500, 40))]
    root = write_schematic(os.path.join(out, name + ".kicad_sch"), name, P, texts, pf, paper="A2", title="Orbie V5 Dock Base Board")
    ptexts = [(0, -12, "ORBIE V5 DOCK r0.1", 1.0, "F_SilkS"), (0, 25, "POGO PINS 14mm", 0.6, "F_SilkS")]
    write_pcb(os.path.join(out, name + ".kicad_pcb"), P, (70, 54, 3), [(-31, 23), (31, 23), (-31, -23), (31, -23)], ptexts, "Orbie V5 Dock Base Board")
    write_project(os.path.join(out, name + ".kicad_pro"), name, root)
    return P

# ============================================================ FEEDER / WATER MODULE BOARDS (dumb, on the 10-pin stacking bus)
BUS = {"1": "+5V", "2": "GND", "3": "FEED_A", "4": "FEED_B", "5": "PUMP_A", "6": "PUMP_B", "7": "DROP_SENSE", "8": "PRESENT_F", "9": "LEVEL_SENSE", "10": "PRESENT_W"}

def feeder_board(out):
    name = "orbie_v5_feeder"
    P = [Part("J1", "Connector", "Conn_01x10_Pin", "BUS from dock (bottom)", ph(10), BUS, (40, 60), 0, (-12, -9, 0), desc="spring-contact bus, bottom face"),
         Part("J3", "Connector", "Conn_01x10_Pin", "BUS to next module (top)", ph(10), BUS, (40, 110), 0, (-12, 9, 0), desc="pass-through so the water tower stacks on top"),
         Part("J2", "Connector", "Conn_01x02_Pin", "PADDLE MOTOR N20", ph(2), {"1": "FEED_A", "2": "FEED_B"}, (120, 60), 0, (12, 0, 90), desc="paddle drum gearmotor"),
         Part("D1", "Device", "LED", "IR LED 940nm (drop beam)", "LED_THT:LED_D3.0mm", {"1": "BEAM_K", "2": "+5V"}, (120, 110), 0, (17, -8, 0), desc="chute emitter"),
         R("R1", "150R", "BEAM_K", "GND", (150, 110), (12, -6, 0), fp=HS_R),
         Part("Q1", "Device", "Q_Photo_NPN", "phototransistor (drop beam)", "LED_THT:LED_D3.0mm", {"1": "GND", "2": "DROP_SENSE"}, (180, 110), 0, (17, 8, 0), desc="chute receiver, pulled up at the dock"),
         R("R2", "0R", "PRESENT_F", "GND", (180, 60), (12, 7, 0), fp=HS_R)]
    texts = [(20, 10, "ORBIE V5 — FEEDER MODULE BOARD 44x26: bus in/out, paddle motor, optical drop sensor across the chute, PRESENT_F strap. No MCU: the dock runs it (REQ-D3).", 2.5)]
    root = write_schematic(os.path.join(out, name + ".kicad_sch"), name, P, texts, [("+5V", (220, 60)), ("GND", (240, 60))], paper="A4", title="Orbie V5 Feeder Module Board")
    write_pcb(os.path.join(out, name + ".kicad_pcb"), P, (44, 26, 2), [(-19, 0), (19, 0)], [(0, 0, "FEEDER r0.1", 0.7, "F_SilkS")], "Orbie V5 Feeder Module Board")
    write_project(os.path.join(out, name + ".kicad_pro"), name, root)
    return P

def water_board(out):
    name = "orbie_v5_water"
    P = [Part("J1", "Connector", "Conn_01x10_Pin", "BUS from feeder/dock", ph(10), BUS, (40, 60), 0, (-12, -9, 0), desc="spring-contact bus, bottom face"),
         Part("J2", "Connector", "Conn_01x02_Pin", "PUMP 5V", ph(2), {"1": "PUMP_A", "2": "PUMP_B"}, (120, 60), 0, (12, -1, 90), desc="submersible pump"),
         Part("J4", "Connector", "Conn_01x02_Pin", "FLOAT SWITCH (low level)", ph(2), {"1": "LEVEL_SENSE", "2": "GND"}, (120, 110), 0, (4, 9, 0), desc="closes when reservoir low"),
         R("R2", "0R", "PRESENT_W", "GND", (180, 60), (16, 8, 0), fp=HS_R)]
    texts = [(20, 10, "ORBIE V5 — WATER MODULE BOARD 44x26: bus in, pump, float switch, PRESENT_W strap. Nothing electrical in the water path (REQ-D5).", 2.5)]
    root = write_schematic(os.path.join(out, name + ".kicad_sch"), name, P, texts, [("+5V", (220, 60)), ("GND", (240, 60))], paper="A4", title="Orbie V5 Water Module Board")
    write_pcb(os.path.join(out, name + ".kicad_pcb"), P, (44, 26, 2), [(-19, 0), (19, 0)], [(0, 0, "WATER r0.1", 0.7, "F_SilkS")], "Orbie V5 Water Module Board")
    write_project(os.path.join(out, name + ".kicad_pro"), name, root)
    return P

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    only = sys.argv[2] if len(sys.argv) > 2 else "all"
    os.makedirs(out, exist_ok=True)
    if only in ("all", "body"): print("== body"); write_bom(out, "orbie_v5_body", body_board(out))
    if only in ("all", "pogo"): print("== pogo"); write_bom(out, "orbie_v5_pogo", pogo_board(out))
    if only in ("all", "dock"): print("== dock"); write_bom(out, "orbie_v5_dock", dock_board(out))
    if only in ("all", "feeder"): print("== feeder"); write_bom(out, "orbie_v5_feeder", feeder_board(out))
    if only in ("all", "water"): print("== water"); write_bom(out, "orbie_v5_water", water_board(out))
    print("done")

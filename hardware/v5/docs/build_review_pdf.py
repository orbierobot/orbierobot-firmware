#!/usr/bin/env python3
"""Build the engineer review PDF: markdown docs + board renders + schematics -> HTML -> Chrome -> PDF."""
import os, re, base64, subprocess, html, datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "kicad", "v5", "out")
DOCS = os.path.join(ROOT, "docs")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

def img(path, width="100%", caption=None):
    with open(path, "rb") as f:
        b = base64.b64encode(f.read()).decode()
    ext = "png" if path.endswith(".png") else "jpeg"
    cap = f'<div class="cap">{html.escape(caption)}</div>' if caption else ""
    return f'<figure><img src="data:image/{ext};base64,{b}" style="width:{width}">{cap}</figure>'

def md(text):
    """Minimal markdown -> HTML: headings, tables, bullets, numbered lists, paragraphs, bold, code, italics."""
    out, lines, i = [], text.splitlines(), 0
    def inline(s):
        s = html.escape(s)
        s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
        s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
        s = re.sub(r"(?<![\w*])\*(?!\s)([^*]+?)\*(?!\w)", r"<i>\1</i>", s)
        return s
    while i < len(lines):
        l = lines[i]
        if l.startswith("# "): out.append(f"<h1>{inline(l[2:])}</h1>")
        elif l.startswith("## "): out.append(f"<h2>{inline(l[3:])}</h2>")
        elif l.startswith("### "): out.append(f"<h3>{inline(l[4:])}</h3>")
        elif l.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")]); i += 1
            rows = [r for r in rows if not all(re.fullmatch(r":?-+:?", c) for c in r)]
            t = "<table><thead><tr>" + "".join(f"<th>{inline(c)}</th>" for c in rows[0]) + "</tr></thead><tbody>"
            for r in rows[1:]: t += "<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>"
            out.append(t + "</tbody></table>"); continue
        elif l.startswith("- "):
            items = []
            while i < len(lines) and lines[i].startswith("- "): items.append(lines[i][2:]); i += 1
            out.append("<ul>" + "".join(f"<li>{inline(x)}</li>" for x in items) + "</ul>"); continue
        elif re.match(r"^\d+\. ", l):
            items = []
            while i < len(lines) and re.match(r"^\d+\. ", lines[i]): items.append(re.sub(r"^\d+\. ", "", lines[i])); i += 1
            out.append("<ol>" + "".join(f"<li>{inline(x)}</li>" for x in items) + "</ol>"); continue
        elif l.startswith("```"):
            buf = []; i += 1
            while i < len(lines) and not lines[i].startswith("```"): buf.append(lines[i]); i += 1
            out.append("<pre>" + html.escape("\n".join(buf)) + "</pre>")
        elif l.strip() == "": pass
        elif l.startswith("*") and l.endswith("*"): out.append(f"<p class='meta'>{inline(l.strip('*'))}</p>")
        else: out.append(f"<p>{inline(l)}</p>")
        i += 1
    return "\n".join(out)

def read(p): return open(p, encoding="utf-8").read()

coverage = read(os.path.join(DOCS, "V5_SPEC_COVERAGE.md"))
retrofit = read(os.path.join(DOCS, "V5_BODY_RETROFIT.md"))
# drop the H1 of each (we add our own section titles)
coverage = re.sub(r"^# .*\n", "", coverage, count=1); retrofit = re.sub(r"^# .*\n", "", retrofit, count=1)

boards = [
    ("orbie_v5_body", "Body controller r0.2", "V3 Main PCB outline 79.5 × 94.62 mm (outline to be confirmed), 2-layer, 1481 tracks, DRC 0, ERC 0",
     "XIAO RP2040 motion MCU · LSM6DS3 IMU · ADS1015 (cliff IR ×2, motor current ×2) · DRV8833 locked-antiphase + 74LVC1G04 ×2 · GA12-N20 encoder ports ×2 · BQ24074 charger/power path · DW01A + AO3400A ×2 pack protection · NTC port · LC709203F gauge · MT3608 5 V · AP2112K 3V3 · TCA9517A I2C buffer · TCA9534 expander · VL53L5CX ports ×3 · MAX98357A + gain strap · USB-C 6P + TVS · pogo input · LD19 lidar port · servo P-FET · hall · 38 kHz IR receiver · RESET/BOOT side buttons · 16-pin neck harness"),
    ("orbie_v5_dock", "Dock base r0.1", "70 × 54 mm, 2-layer, 528 tracks, DRC 0, ERC 0",
     "XIAO ESP32-C3 · USB-C 5 V in + TVS + 3 A polyfuse · AO3401A pad switch with 100 k pull-down and 0.05 Ω shunt read by the C3 ADC (pads dead until hall + handshake) · P50 pogo pins at 14 mm · IR beacon LEDs ×2 (L/R lobes, 38 kHz) · AH1806 hall · DRV8833 for feeder motor + pump · TCA9534 · 10-pin stacking bus · feed/pair button · status LED"),
    ("orbie_v5_feeder", "Feeder module r0.1", "44 × 26 mm, 2-layer, DRC 0", "bus in (bottom) and bus out (top) for stacking · paddle-motor JST · 940 nm drop-beam LED + phototransistor across the chute · PRESENT_F strap. No MCU."),
    ("orbie_v5_water", "Water module r0.1", "44 × 26 mm, 2-layer, DRC 0", "bus in · pump JST · float switch (low level) · PRESENT_W strap. Nothing electrical in the water path."),
    ("orbie_v5_pogo", "Belly pogo pad board r0.1", "28 × 16 mm, 2-layer, DRC 0", "two Ø6 mm gold pads at 14 mm, facing down through a window in the body floor; JST to the body J4"),
]

parts = []
parts.append(f"""
<div class="cover">
  <div class="kicker">Orbie · hardware review package</div>
  <h1 class="big">Orbie V5 electronics<br>what was done, how, and what to verify</h1>
  <p class="meta">Ayva Labs / Orbie · {datetime.date.today().strftime('%d %B %Y')} · Body board rev 0.2, dock / feeder / water / pogo rev 0.1<br>
  Status: five boards placed, autorouted and DRC-clean; fab files generated; <b>not yet reviewed by an engineer, not yet ordered</b>.<br>
  Licence: CERN-OHL-S-2.0 (hardware) · Generated designs, source in <code>hardware/v5/</code> of this repository.</p>
  <h3>Why this exists</h3>
  <p>The V3 prototype tips forward: its body sits on top of the wheel axle and the heavy head sits above that, so the centre of gravity is about 55 mm above the axle. V5 keeps the V3 shell and face and makes the robot an actively balanced two-wheeler, like Loona, with a real-time motion MCU in the body, encoder motors, an IMU, and the dock, battery-management and sensing items from the 26 September 2026 hardware-requirements document. This package is the electronics for that: a body controller that retrofits the V3 body, a belly pogo pad, a dock base, and two plug-in dock modules.</p>
  <h3>What we ask the reviewers</h3>
  <ol>
    <li>Confirm the body-board outline against the body you have (section 6: the March STEP does not contain the January "Main PCB").</li>
    <li>Check the five component values flagged in section 7 against current datasheets.</li>
    <li>Eyeball the autorouted switching loops (MT3608, DRV8833) and the two sense-resistor connections.</li>
    <li>Decide 1S2P vs 2S, ballast vs lighter head, and nose-in vs reverse-in docking (section 6).</li>
  </ol>
</div>
<div class="pb"></div>
""")
parts.append("<h1>1. Where V5 came from</h1>")
parts.append("<p>Left: the V3 body as built (barrel on the axle, 120 mm head, caster and front skids). Right: the same robot family re-cut as a wheeled balancer. The painted prototype of 29 March 2026 is the body this package retrofits.</p>")
parts.append(img(os.path.join(ROOT, "ref", "proto_montage.png"), "100%", "V3 painted prototype, 29 Mar 2026, with the front skids added to stop it falling forward"))
parts.append(md("""
## Architecture: two brains

- **Head (unchanged V3 head board):** XIAO ESP32-S3 Sense. Camera, Wi-Fi/BLE, dot-matrix face, laser, servo PWM, I2S audio out, app API, OTA.
- **Body (new board):** XIAO RP2040. 1 kHz balance loop from the IMU, quadrature encoders on PIO, locked-antiphase PWM into the DRV8833, cliff and current sensing through an I2C ADC, bumper input. Motor safety timeouts live here and never on anything Linux-based (REQ-C3).
- **Link:** one UART over the neck harness. The head sends velocity setpoints; the body answers with pitch, speed, battery and faults. A future Linux SoC for the pet-health models talks to the same UART.
- **Dock:** XIAO ESP32-C3 with its own Wi-Fi. Feeding runs on schedule whether or not the robot is alive. Modules are dumb.
"""))
parts.append("<div class='pb'></div><h1>2. The five boards</h1>")
for name, title, status, contents in boards:
    parts.append(f"<h2>{html.escape(title)} <span class='small'>({name})</span></h2><p class='meta'>{html.escape(status)}</p><p>{html.escape(contents)}</p>")
    top = os.path.join(OUT, f"{name}.top.png")
    if os.path.exists(top): parts.append(img(top, "62%" if name == "orbie_v5_body" else "78%", f"{title}: top side, autorouted, ground pours on both layers"))
    if name == "orbie_v5_body":
        bot = os.path.join(OUT, f"{name}.bottom.png")
        if os.path.exists(bot): parts.append(img(bot, "62%", "Body controller: bottom side (hall sensor only; motor-clamp zones kept clear)"))
    parts.append("<div class='pb'></div>")
parts.append("<h1>3. Requirement coverage, retrofit and build notes</h1>")
parts.append(md(coverage))
parts.append("<div class='pb'></div><h1>4. Retrofit into the V3 body, harness and order list</h1>")
parts.append(md(retrofit))
parts.append("<div class='pb'></div><h1>5. Verification performed</h1>")
parts.append(md("""
| Check | Tool | Result |
|---|---|---|
| Electrical rules (every pin assigned or marked no-connect; power pins driven) | KiCad 10 ERC, all severities | body 0, dock 0, feeder 0, water 0, pogo 0 errors |
| Design rules after routing (clearance 0.15 mm, track 0.2 mm, via 0.6/0.3, courtyards, edge clearance, mask bridges, starved thermals) | KiCad 10 DRC, all severities | 0 errors on all five boards |
| Unrouted connections | KiCad DRC | 0 on all five boards |
| Outline and hole pattern | read 1:1 from the V3 "Main PCB" drawing DXF | matched; **but see section 6** |
| Mechanical keep-outs from the drawing (7.5 mm over the waist, 1 mm on the bottom motor-clamp zones) | manual | respected |
| 3D fit of encoder motors, harness entry, pogo window | — | **not done** |
| Datasheet check of computed values | — | **not done** |
| Firmware for RP2040 / dock C3 / head changes | — | **not written** |
"""))
parts.append("<h1>6. The outline question (blocker for the body board)</h1>")
parts.append(md("""
The body board copies the outline and the four M3 holes from the drawing *Main PCB* (24 Jan 2026): 79.5 × 94.62 mm, 63.74 mm waist, holes at 18.6 × 40 mm. To verify it I loaded the assembly STEP of 8 March 2026 (`Pawme_Assembly.STEP`, 639 solids) in FreeCAD and looked for that board and for screw bosses on that pattern.

- There is **no** 79.5 × 94.62 board in the March assembly. The body contains the ESP-ROLL board (`Rolling+Robot+Top+Board`, 73.0 × 42.7 mm) and the display board (80 × 36 mm).
- No boss pattern in `Body_Top`, `BodyTop_Subassembly` or `Support_Subassembly` matches 18.6 × 40 mm. Patterns found: 62 × 44.8 mm (r 1.1) on the battery support, 47.4 × 46.8 mm (r 1.4) and 75 × 80 mm (r 1.32) on the top shell.
- The March assembly also shows **two** 18650 cells side by side (so 1S2P is what the body already holds), a 42 × 22 × 52 speaker, and Ø89.8 mm wheels on a 129 mm track.

So either the body you have was built to the March CAD (then the board must be re-outlined to its mounts), or the January Main PCB posts exist in a later shell revision. Please tell us which, or send the current `Body_Bottom` STEP. The outline is one function in the generator; re-cutting it is a ten-minute change and re-routing is automatic.
"""))
parts.append("<div class='pb'></div><h1>7. Review checklist for the engineers</h1>")
parts.append(md("""
1. **Outline / mounting** (section 6). Blocking for the body board only.
2. **BQ24074**: ILIM = 1k1 (1.5 A), ISET = 590 Ω (1.5 A), ITERM = 1k5, TMR = 10 k, and TS with a 10 k NTC (0–45 °C window; R9 bypass marked DNP). Confirm against the current datasheet revision and the cells chosen.
3. **DW01A** thresholds (2.4 V / 4.3 V / 150 mV OC) against the cells; AO3400A as the protection switches (5.7 A).
4. **ADS1015** input range for the 0.1 Ω motor sense (use the ±256 mV PGA) and for the cliff IR analog outputs (0–3.3 V, ±4.096 V range).
5. **MT3608**: 75 k / 22 k feedback → 5.0 V; 4.7 µH ≥ 3 A inductor; 22 µF in/out.
6. **Locked-antiphase drive**: 20 kHz PWM, expect idle current ripple; confirm the N20 encoder motors accept it, or switch firmware to sign-magnitude on the same hardware.
7. **Switching loops**: U8/L1/D3/C11 and U3/C4 were placed tight but autorouted; check loop area and that AISEN/BISEN sense returns are Kelvin to the shunt pads.
8. **Neck harness**: 16 wires through the neck; confirm the V3 head board can expose XIAO RST/BOOT (pins 14/15) and GPIO4 for the lidar.
9. **Dock pads**: AO3401A + shunt + firmware fold-back is the safety path for REQ-D1; decide whether a dedicated eFuse (TPS2553 class) is wanted for production.
10. **Thermal**: nothing on these boards addresses the 58 °C shell reading; the companion-SoC decision drives it.
"""))
parts.append("<h1>8. How these boards were designed</h1>")
parts.append(md("""
- **Parts** were chosen from working knowledge of the common parts' datasheets (TI BQ2407x, DRV8833, TCA9534/9517; Maxim MAX98357A; ST LSM6DS3, VL53L5CX; onsemi LC709203F; Fortune DW01A; Seeed XIAO pinouts). Datasheets were not fetched live; computed values are listed above for checking.
- **Symbols and footprints** are the stock KiCad 10 libraries (pin names, pin numbers, package dimensions from the manufacturers' drawings). A Python model (`gen_v5.py`) assigns every pin a net by name, so a wrong pin name fails at generation time.
- **Placement** was done by hand in millimetres in the generator and iterated against DRC until courtyards, holes and edges were clean.
- **Routing**: freerouting (autorouter) via Specctra DSN/SES, then GND pours on both layers, then DRC again, to JLCPCB 2-layer limits (0.127 mm track/space, 0.3 mm drill).
- **Outputs**: `kicad-cli` gerbers including paste layers (stencil), Excellon drill, pick-and-place CSV, JLC-format BOM (LCSC column empty, MPN filled).
- Everything is regenerable: edit the generator, re-run, re-route. Do not hand-edit the outputs unless you also stop using the generator.
"""))
parts.append("<div class='pb'></div><h1>Appendix A. Schematics</h1><p>Full-size PDFs are in <code>hardware/v5/out/*.sch.pdf</code>; these pages are previews.</p>")
for name, title, _, _ in boards:
    p = os.path.join(OUT, f"sch_{name}-1.png")
    if os.path.exists(p): parts.append(f"<h2>{html.escape(title)}</h2>" + img(p, "100%", f"{name}.kicad_sch")); parts.append("<div class='pb'></div>")
parts.append("<h1>Appendix B. File index</h1>")
parts.append(md("""
| Path (repo: hardware/v5/) | What |
|---|---|
| `gen_v5.py`, `gen_kicad.py`, `route_v5.py`, `orbie_v5.pretty/` | generator, library helpers, router/fab script, custom footprints (XIAO castellated, pogo pad) |
| `orbie_v5_<board>.kicad_pro/.kicad_sch/.kicad_pcb` | KiCad 10 projects, routed |
| `out/<board>_gerbers.zip` | gerbers (F/B Cu, paste, mask, silk, edge), drill, job file |
| `out/<board>_pos.csv`, `out/<board>_bom_jlc.csv` | pick-and-place, BOM |
| `out/<board>.sch.pdf`, `*.top.png`, `*.bottom.png` | schematic PDF, 3D renders |
| `out/<board>.erc.json`, `*.drc.json` | check reports |
| `docs/V5_SPEC_COVERAGE.md`, `docs/V5_BODY_RETROFIT.md` | sources of sections 3 and 4 |
"""))

css = """
@page { size: A4; margin: 16mm 14mm 16mm 14mm; }
body { font-family: -apple-system, "Helvetica Neue", Helvetica, Arial, sans-serif; font-size: 10.2pt; line-height: 1.38; color: #1b1b1b; }
h1 { font-size: 17pt; margin: 0 0 6pt; padding-bottom: 3pt; border-bottom: 2px solid #f28c28; }
h2 { font-size: 13pt; margin: 14pt 0 4pt; color: #222; }
h3 { font-size: 11pt; margin: 10pt 0 3pt; }
p { margin: 4pt 0 6pt; }
.meta { color: #555; font-size: 9pt; }
.small { font-weight: normal; color: #777; font-size: 9pt; }
table { border-collapse: collapse; width: 100%; margin: 6pt 0 10pt; font-size: 8.6pt; page-break-inside: auto; }
th, td { border: 1px solid #cfcfcf; padding: 3pt 5pt; vertical-align: top; text-align: left; }
th { background: #f3f3f1; }
tr { page-break-inside: avoid; }
code { font-family: Menlo, monospace; font-size: 8.6pt; background: #f4f4f2; padding: 0 2pt; }
pre { font-family: Menlo, monospace; font-size: 8pt; background: #f4f4f2; padding: 6pt; white-space: pre-wrap; }
figure { margin: 6pt 0 10pt; text-align: center; page-break-inside: avoid; }
figure img { max-width: 100%; border: 1px solid #e3e3e3; }
.cap { font-size: 8.6pt; color: #555; margin-top: 3pt; }
.pb { page-break-after: always; }
.cover { padding-top: 40mm; }
.kicker { color: #f28c28; font-weight: 600; letter-spacing: .08em; text-transform: uppercase; font-size: 9pt; }
.big { font-size: 26pt; border: none; line-height: 1.15; margin: 6pt 0 14pt; }
ul, ol { margin: 2pt 0 8pt 18pt; } li { margin: 2pt 0; }
"""
doc = f"<!doctype html><html><head><meta charset='utf-8'><title>Orbie V5 hardware review</title><style>{css}</style></head><body>{''.join(parts)}</body></html>"
html_path = os.path.join(DOCS, "Orbie_V5_Hardware_Review.html")
pdf_path = os.path.join(DOCS, "Orbie_V5_Hardware_Review.pdf")
open(html_path, "w", encoding="utf-8").write(doc)
r = subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-pdf-header-footer", "--no-margins", f"--print-to-pdf={pdf_path}", f"file://{html_path}"], capture_output=True, text=True, timeout=180)
print("chrome rc", r.returncode, r.stderr[-300:])
print("pdf", os.path.getsize(pdf_path) if os.path.exists(pdf_path) else "MISSING")

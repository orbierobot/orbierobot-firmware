#!/usr/bin/env python3
"""
Generate KiCad 9/10 projects (schematic + PCB) for the Orbie V4 redesign
from a single Python data model, using the stock KiCad symbol/footprint libs.

  * Schematic: symbols copied (flattened) from the stock .kicad_sym libraries,
    every used pin gets a global label with its net name, unused pins get
    no-connect markers, power nets get PWR_FLAGs.
  * PCB: board outline, mounting holes, footprints placed and pads assigned
    to nets (ratsnest ready for routing).  Run with KiCad's bundled python
    so that `pcbnew` imports.

Usage:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 gen_kicad.py <out_dir>
"""
import os, re, sys, uuid, json, math, copy

KICAD = "/Applications/KiCad/KiCad.app/Contents/SharedSupport"
LAYERS = 4
LOCAL_FP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "orbie_v5.pretty")
SYMDIR = os.path.join(KICAD, "symbols")
FPDIR = os.path.join(KICAD, "footprints")

# ============================================================ s-expression I/O
class Atom:
    __slots__ = ("t", "q")
    def __init__(self, t, q): self.t = t; self.q = q
    def __repr__(self): return f'"{self.t}"' if self.q else self.t

def tokenize(s):
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c.isspace(): i += 1; continue
        if c in "()": yield c; i += 1; continue
        if c == '"':
            j = i + 1; buf = []
            while j < n:
                if s[j] == "\\" and j + 1 < n: buf.append(s[j:j+2]); j += 2; continue
                if s[j] == '"': break
                buf.append(s[j]); j += 1
            yield Atom("".join(buf), True); i = j + 1; continue
        j = i
        while j < n and not s[j].isspace() and s[j] not in "()": j += 1
        yield Atom(s[i:j], False); i = j

def parse(s):
    stack = [[]]
    for tok in tokenize(s):
        if tok == "(": stack.append([])
        elif tok == ")":
            lst = stack.pop(); stack[-1].append(lst)
        else: stack[-1].append(tok)
    return stack[0]

def ser(node, ind=0):
    if isinstance(node, Atom): return repr(node)
    if not node: return "()"
    head = node[0]
    simple = all(isinstance(x, Atom) for x in node)
    if simple:
        return "(" + " ".join(repr(x) for x in node) + ")"
    out = "(" + (repr(head) if isinstance(head, Atom) else ser(head, ind + 1))
    for x in node[1:]:
        if isinstance(x, Atom): out += " " + repr(x)
        else: out += "\n" + "\t" * (ind + 1) + ser(x, ind + 1)
    return out + "\n" + "\t" * ind + ")"

def A(t): return Atom(str(t), False)
def Q(t): return Atom(str(t), True)
def L(*xs): return list(xs)
def find(node, key):
    return [x for x in node if isinstance(x, list) and x and isinstance(x[0], Atom) and x[0].t == key]
def find1(node, key):
    r = find(node, key); return r[0] if r else None
def uid(): return str(uuid.uuid4())

# ============================================================ symbol library access
_libcache = {}
def load_lib(lib):
    if lib not in _libcache:
        with open(os.path.join(SYMDIR, lib + ".kicad_sym"), encoding="utf-8") as f:
            _libcache[lib] = parse(f.read())[0]
    return _libcache[lib]

def raw_symbol(lib, name):
    for s in find(load_lib(lib), "symbol"):
        if s[1].t == name: return s
    raise KeyError(f"{lib}:{name}")

def flatten_symbol(lib, name):
    """Return a deep-copied, flattened (no `extends`) symbol node named lib:name."""
    sym = copy.deepcopy(raw_symbol(lib, name))
    ext = find1(sym, "extends")
    if ext:
        parent = flatten_symbol(lib, ext[1].t)
        # parent already renamed to lib:parent -> rename to lib:name, override properties
        props = {p[1].t: p for p in find(sym, "property")}
        out = [x for x in parent if not (isinstance(x, list) and x and x[0].t == "property" and x[1].t in props)]
        # keep the parent's property position/effects, but derived text
        for p in find(parent, "property"):
            if p[1].t in props:
                np = copy.deepcopy(p); np[2] = Q(props[p[1].t][2].t); out.append(np)
        # re-order: header atoms first, then everything else
        sym = out
        # sub-symbol names inside parent are <parent>_u_s ; KiCad accepts any names but
        # convention is <name>_u_s -> rename
        pname = ext[1].t
        for sub in find(sym, "symbol"):
            sub[1] = Q(sub[1].t.replace(pname, name, 1))
    sym[1] = Q(f"{lib}:{name}")
    return sym

def symbol_pins(symnode, unit=1):
    """(number, name, x, y, angle, etype) for unit `unit` incl. common unit 0."""
    pins = []
    for sub in find(symnode, "symbol"):
        m = re.match(r".*_(\d+)_(\d+)$", sub[1].t)
        if not m: continue
        u = int(m.group(1))
        if u not in (0, unit): continue
        for p in find(sub, "pin"):
            at = find1(p, "at"); nm = find1(p, "name"); num = find1(p, "number")
            pins.append((num[1].t, nm[1].t, float(at[1].t), float(at[2].t),
                         float(at[3].t) if len(at) > 3 else 0.0, p[1].t))
    return pins

# ============================================================ data model
class Part:
    def __init__(self, ref, lib, sym, value, footprint, nets, pos, rot=0, pcb=None, side="F", desc="", pad_nets=None):
        self.pad_nets = pad_nets
        self.ref, self.lib, self.sym, self.value = ref, lib, sym, value
        self.footprint = footprint            # "LibName:FootprintName"
        self.nets = nets                      # {pin_number(str): net}
        self.pos = pos                        # schematic (x, y) mm
        self.rot = rot
        self.pcb = pcb                        # (x, y, rot) mm on board
        self.side = side
        self.desc = desc
        self.uuid = uid()

def label_angle(pin_angle):
    return {0: 180, 180: 0, 90: 270, 270: 90}[int(pin_angle) % 360]

def rot_pt(x, y, deg):
    r = math.radians(deg); c, s = math.cos(r), math.sin(r)
    return x * c - y * s, x * s + y * c

# ============================================================ schematic writer
def write_schematic(path, project, parts, texts, power_flags, paper="A2", title="", rev="0.1"):
    root_uuid = uid()
    lib_symbols = [A("lib_symbols")]
    seen = set()
    body = []
    for p in parts:
        key = f"{p.lib}:{p.sym}"
        if key not in seen:
            lib_symbols.append(flatten_symbol(p.lib, p.sym)); seen.add(key)
        symnode = lib_symbols[[i for i, s in enumerate(lib_symbols) if isinstance(s, list) and s[1].t == key][0]]
        X, Y = p.pos
        inst = [A("symbol"), L(A("lib_id"), Q(key)), L(A("at"), A(X), A(Y), A(p.rot)), L(A("unit"), A(1)),
                L(A("exclude_from_sim"), A("no")), L(A("in_bom"), A("yes")), L(A("on_board"), A("yes")),
                L(A("dnp"), A("no")), L(A("fields_autoplaced"), A("yes")), L(A("uuid"), Q(p.uuid))]
        pins = symbol_pins(symnode)
        xs = [q[2] for q in pins] or [0]; ys = [q[3] for q in pins] or [0]
        top = max(ys) + 2.54; bot = min(ys) - 2.54
        def prop(name, val, dy, hide=False):
            eff = [A("effects"), L(A("font"), L(A("size"), A(1.27), A(1.27)))]
            if hide: eff.append(L(A("hide"), A("yes")))
            return [A("property"), Q(name), Q(val), L(A("at"), A(X), A(round(Y - dy, 2)), A(0)), eff]
        inst.append(prop("Reference", p.ref, top + 1.27))
        inst.append(prop("Value", p.value, bot - 1.27))
        inst.append(prop("Footprint", p.footprint, bot - 3.81, hide=True))
        inst.append(prop("Datasheet", "~", bot - 6.35, hide=True))
        inst.append(prop("Description", p.desc, bot - 8.89, hide=True))
        for num, name, px, py, ang, et in pins:
            inst.append([A("pin"), Q(num), L(A("uuid"), Q(uid()))])
        inst.append([A("instances"), [A("project"), Q(project),
                     [A("path"), Q("/" + root_uuid), L(A("reference"), Q(p.ref)), L(A("unit"), A(1))]]])
        body.append(inst)
        # labels / no-connects at pin ends
        for num, name, px, py, ang, et in pins:
            rx, ry = rot_pt(px, py, p.rot)
            gx, gy = round(X + rx, 3), round(Y - ry, 3)
            net = p.nets.get(num)
            if net is None:
                body.append([A("no_connect"), L(A("at"), A(gx), A(gy)), L(A("uuid"), Q(uid()))])
                continue
            la = label_angle(ang + p.rot)
            just = "left" if la in (0, 90) else "right"
            body.append([A("global_label"), Q(net), L(A("shape"), A("bidirectional")),
                         L(A("at"), A(gx), A(gy), A(la)), L(A("fields_autoplaced"), A("yes")),
                         [A("effects"), L(A("font"), L(A("size"), A(1.27), A(1.27))), L(A("justify"), A(just))],
                         L(A("uuid"), Q(uid())),
                         [A("property"), Q("Intersheetrefs"), Q("${INTERSHEET_REFS}"), L(A("at"), A(gx), A(gy), A(0)),
                          [A("effects"), L(A("font"), L(A("size"), A(1.27), A(1.27))), L(A("hide"), A("yes"))]]])
    # power flags: PWR_FLAG symbol with its pin at a global label
    if "power:PWR_FLAG" not in seen:
        lib_symbols.append(flatten_symbol("power", "PWR_FLAG"))
    for i, (net, (X, Y)) in enumerate(power_flags):
        ref = f"#FLG{i+1:02d}"
        pf = [A("symbol"), L(A("lib_id"), Q("power:PWR_FLAG")), L(A("at"), A(X), A(Y), A(0)), L(A("unit"), A(1)),
              L(A("exclude_from_sim"), A("no")), L(A("in_bom"), A("yes")), L(A("on_board"), A("yes")), L(A("dnp"), A("no")),
              L(A("uuid"), Q(uid())),
              [A("property"), Q("Reference"), Q(ref), L(A("at"), A(X), A(Y - 4), A(0)),
               [A("effects"), L(A("font"), L(A("size"), A(1.27), A(1.27))), L(A("hide"), A("yes"))]],
              [A("property"), Q("Value"), Q("PWR_FLAG"), L(A("at"), A(X), A(Y - 2.5), A(0)),
               [A("effects"), L(A("font"), L(A("size"), A(1.27), A(1.27)))]],
              [A("pin"), Q("1"), L(A("uuid"), Q(uid()))],
              [A("instances"), [A("project"), Q(project), [A("path"), Q("/" + root_uuid), L(A("reference"), Q(ref)), L(A("unit"), A(1))]]]]
        body.append(pf)
        body.append([A("global_label"), Q(net), L(A("shape"), A("bidirectional")), L(A("at"), A(X), A(Y), A(0)),
                     [A("effects"), L(A("font"), L(A("size"), A(1.27), A(1.27))), L(A("justify"), A("left"))],
                     L(A("uuid"), Q(uid()))])
    for (tx, ty, txt, size) in texts:
        body.append([A("text"), Q(txt), L(A("exclude_from_sim"), A("no")), L(A("at"), A(tx), A(ty), A(0)),
                     [A("effects"), L(A("font"), L(A("size"), A(size), A(size)), A("bold")), L(A("justify"), A("left"))],
                     L(A("uuid"), Q(uid()))])
    sch = [A("kicad_sch"), L(A("version"), A(20250114)), L(A("generator"), Q("orbie_gen")), L(A("generator_version"), Q("9.0")),
           L(A("uuid"), Q(root_uuid)), L(A("paper"), Q(paper)),
           [A("title_block"), L(A("title"), Q(title)), L(A("date"), Q("2026-09-24")), L(A("rev"), Q(rev)),
            L(A("company"), Q("Orbie / Ayva Labs — CERN-OHL-S-2.0"))],
           lib_symbols] + body + [[A("sheet_instances"), [A("path"), Q("/"), L(A("page"), Q("1"))]]]
    with open(path, "w", encoding="utf-8") as f:
        f.write(ser(sch) + "\n")
    return root_uuid

def write_project(path, name, root_uuid):
    pro = {"board": {"design_settings": {"defaults": {}, "rules": {"min_clearance": 0.15, "min_track_width": 0.15,
                                                                     "min_via_diameter": 0.5, "min_via_drill": 0.3}}},
           "meta": {"filename": f"{name}.kicad_pro", "version": 3},
           "net_settings": {"classes": [{"name": "Default", "clearance": 0.127, "track_width": 0.2, "via_diameter": 0.6, "via_drill": 0.3, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "microvia_diameter": 0.3, "microvia_drill": 0.1, "wire_width": 6, "bus_width": 12, "line_style": 0, "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)"}], "meta": {"version": 4}},
           "pcbnew": {"page_layout_descr_file": ""}, "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
           "sheets": [[root_uuid, "Root"]], "text_variables": {}}
    with open(path, "w") as f: json.dump(pro, f, indent=2)

# ============================================================ PCB writer (pcbnew)
def write_pcb(path, parts, outline, holes, texts, title, hole_fp="MountingHole_2.2mm_M2_Pad"):
    import pcbnew
    from pcbnew import VECTOR2I, FromMM
    board = pcbnew.BOARD()
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(LAYERS)
    # outline: rounded rectangle (w,h,r) or explicit polygon points
    if isinstance(outline, tuple) and len(outline) == 3:
        w, h, r = outline
        pts = []
        for cx, cy, a0 in ((w/2-r, h/2-r, 0), (-w/2+r, h/2-r, 90), (-w/2+r, -h/2+r, 180), (w/2-r, -h/2+r, 270)):
            for k in range(0, 10):
                a = math.radians(a0 + k * 9)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    else:
        pts = list(outline)
    for i in range(len(pts)):
        x0, y0 = pts[i]; x1, y1 = pts[(i + 1) % len(pts)]
        seg = pcbnew.PCB_SHAPE(board); seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(VECTOR2I(FromMM(x0), FromMM(-y0))); seg.SetEnd(VECTOR2I(FromMM(x1), FromMM(-y1)))
        seg.SetLayer(pcbnew.Edge_Cuts); seg.SetWidth(FromMM(0.1)); board.Add(seg)
    nets = {}
    def net(name):
        if name not in nets:
            ni = pcbnew.NETINFO_ITEM(board, name); board.Add(ni); nets[name] = ni
        return nets[name]
    for (hx, hy) in holes:
        fp = pcbnew.FootprintLoad(os.path.join(FPDIR, "MountingHole.pretty"), hole_fp)
        fp.SetPosition(VECTOR2I(FromMM(hx), FromMM(-hy))); fp.SetReference(f"H{len(nets)+1}"); board.Add(fp)
        nets[f"__h{hx}{hy}"] = None
    for p in parts:
        if not p.pcb: continue
        lib, name = p.footprint.split(":")
        fp = pcbnew.FootprintLoad(LOCAL_FP if lib == "orbie_v5" else os.path.join(FPDIR, lib + ".pretty"), name)
        if fp is None: raise SystemExit(f"footprint missing {p.footprint}")
        fp.SetReference(p.ref); fp.SetValue(p.value)
        fp.SetPath(pcbnew.KIID_PATH("/" + p.uuid))
        board.Add(fp)
        x, y, rot = p.pcb
        if p.side == "B":
            fp.SetLayerAndFlip(pcbnew.B_Cu)
        fp.SetPosition(VECTOR2I(FromMM(x), FromMM(-y)))
        fp.SetOrientationDegrees(rot)
        pn = p.pad_nets if p.pad_nets else p.nets
        for pad in fp.Pads():
            n = pn.get(pad.GetNumber())
            if n: pad.SetNet(net(n))
    for (tx, ty, txt, size, layer) in texts:
        t = pcbnew.PCB_TEXT(board); t.SetText(txt); t.SetPosition(VECTOR2I(FromMM(tx), FromMM(-ty)))
        t.SetLayer(getattr(pcbnew, layer)); t.SetTextSize(VECTOR2I(FromMM(size), FromMM(size))); t.SetTextThickness(FromMM(size * 0.15))
        if layer.startswith("B_"): t.SetMirrored(True)
        board.Add(t)
    board.GetTitleBlock().SetTitle(title); board.GetTitleBlock().SetRevision("0.1"); board.GetTitleBlock().SetDate("2026-09-24")
    pcbnew.SaveBoard(path, board)

# ============================================================ common helpers for parts
def two_pin(ref, lib, sym, value, fp, n1, n2, pos, rot=0, pcb=None, side="F", desc=""):
    return Part(ref, lib, sym, value, fp, {"1": n1, "2": n2}, pos, rot, pcb, side, desc)

def C(ref, val, n1, n2, pos, pcb=None, side="F", fp="Capacitor_SMD:C_0402_1005Metric"):
    return two_pin(ref, "Device", "C", val, fp, n1, n2, pos, 0, pcb, side, "capacitor")
def R(ref, val, n1, n2, pos, pcb=None, side="F", fp="Resistor_SMD:R_0402_1005Metric"):
    return two_pin(ref, "Device", "R", val, fp, n1, n2, pos, 0, pcb, side, "resistor")

# ============================================================ MAIN BOARD
def main_board(out):
    name = "orbie_v4_main"
    P = []
    T = []  # schematic texts
    # ---------------- column X positions per block
    # ---- MCU: XIAO ESP32-S3 Sense as two 7-pin headers
    T.append((20, 18, "U1  Seeed XIAO ESP32-S3 Sense (camera on FPC to head). Pin map keeps the d_wip firmware map.", 2.0))
    xiao_l = {"1": "GPIO1_MOTA_FWD", "2": "GPIO2_MOTA_REV", "3": "GPIO3_MOTB_REV", "4": "GPIO4_MOTB_FWD",
              "5": "SDA", "6": "SCL", "7": "GPIO43_SERVO_PWM"}
    xiao_r = {"1": "+5V", "2": "GND", "3": "+3V3_XIAO", "4": "GPIO9_IR_RX", "5": "GPIO8_I2S_LRC", "6": "GPIO7_I2S_BCLK", "7": "GPIO44_I2S_DIN"}
    P.append(Part("U1A", "Connector", "Conn_01x07_Pin", "XIAO_ESP32S3_D0-D6", "Connector_PinSocket_2.54mm:PinSocket_1x07_P2.54mm_Vertical",
                  xiao_l, (40, 40), 0, (-7.62, 30, 0), desc="XIAO left row D0..D6"))
    P.append(Part("U1B", "Connector", "Conn_01x07_Pin", "XIAO_ESP32S3_5V-D7", "Connector_PinSocket_2.54mm:PinSocket_1x07_P2.54mm_Vertical",
                  xiao_r, (70, 40), 0, (7.62, 30, 0), desc="XIAO right row 5V,GND,3V3,D10..D7"))
    # ---- I2C bus pull-ups + expander
    P.append(R("R1", "2k2", "+3V3", "SDA", (100, 34), (-12, 12, 0)))
    P.append(R("R2", "2k2", "+3V3", "SCL", (100, 44), (-12, 14, 0)))
    T.append((120, 18, "U3  TCA9534 I/O expander @0x38 — frees the GPIOs the V3 ran out of", 2.0))
    tca = {"1": "SDA", "2": "SCL", "3": "EXP_INT", "4": "EXP_P0_LASER_EN", "5": "EXP_P1_SERVO_PWR_EN", "6": "EXP_P2_DOCK_DET",
           "7": "EXP_P3_CHG_STAT1", "8": "GND", "9": "EXP_P4_CHG_STAT2", "10": "EXP_P5_CHG_PGOOD", "11": "EXP_P6_DRV_nSLEEP",
           "12": "EXP_P7_DRV_nFAULT", "13": "GND", "14": "GND", "15": "GND", "16": "+3V3"}
    # TCA9534 pins: 1 A0,2 A1,3 A2? -> map by name below instead of number
    P.append(Part("U3", "Interface_Expansion", "TCA9534", "TCA9534PWR", "Package_SO:TSSOP-16_4.4x5mm_P0.65mm",
                  {}, (150, 45), 0, (-22, 8, 0), desc="8-bit I2C GPIO expander"))
    P.append(C("C1", "100n", "+3V3", "GND", (185, 34), (-22, 3, 0)))
    # ---- Motor driver
    T.append((20, 90, "U2  DRV8833 dual H-bridge — N20 gearmotors L/R, VM from VSYS (option: +5V boost for more torque)", 2.0))
    drv = {"VM": "VSYS", "GND": "GND", "VCP": "DRV_VCP", "VINT": "DRV_VINT", "AIN1": "GPIO1_MOTA_FWD", "AIN2": "GPIO2_MOTA_REV",
           "BIN1": "GPIO4_MOTB_FWD", "BIN2": "GPIO3_MOTB_REV", "AOUT1": "MOTA_1", "AOUT2": "MOTA_2", "BOUT1": "MOTB_1", "BOUT2": "MOTB_2",
           "AISEN": "GND", "BISEN": "GND", "~{SLEEP}": "EXP_P6_DRV_nSLEEP", "~{FAULT}": "EXP_P7_DRV_nFAULT"}
    P.append(Part("U2", "Driver_Motor", "DRV8833PWP", "DRV8833PWP", "Package_SO:HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3.4x5mm",
                  drv, (60, 120), 0, (-24, -8, 0), desc="dual H-bridge 1.5A"))
    P.append(C("C2", "10u", "VSYS", "GND", (100, 105), (-30, -14, 0), fp="Capacitor_SMD:C_0805_2012Metric"))
    P.append(C("C3", "10n", "DRV_VCP", "VSYS", (100, 115), (-30, -4, 0)))
    P.append(C("C4", "2u2", "DRV_VINT", "GND", (100, 125), (-30, -8, 0)))
    P.append(R("R3", "10k", "+3V3", "EXP_P7_DRV_nFAULT", (100, 135), (-30, -11, 0)))
    P.append(Part("J3", "Connector", "Conn_01x02_Pin", "MOTOR_L", "Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical",
                  {"1": "MOTA_1", "2": "MOTA_2"}, (130, 112), 0, (-36, -2, 90), desc="N20 motor left"))
    P.append(Part("J4", "Connector", "Conn_01x02_Pin", "MOTOR_R", "Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical",
                  {"1": "MOTB_1", "2": "MOTB_2"}, (130, 128), 0, (36, -2, 270), desc="N20 motor right"))
    # ---- IMU
    T.append((200, 90, "U4  LSM6DS3 IMU — pendulum damping loop, tip-over + activity detect (NEW vs V3)", 2.0))
    imu = {"SDO/SA0": "GND", "SDX": None, "SCX": None, "INT1": "IMU_INT1", "VDDIO": "+3V3", "GND": "GND", "VDD": "+3V3",
           "INT2": None, "NC": None, "CS": "+3V3", "SCL": "SCL", "SDA": "SDA"}
    P.append(Part("U4", "Sensor_Motion", "LSM6DS3", "LSM6DS3TR-C", "Package_LGA:LGA-14_3x2.5mm_P0.5mm_LayoutBorder3x4y",
                  {}, (235, 120), 0, (0, 0, 0), desc="6-axis IMU"))
    P.append(C("C5", "100n", "+3V3", "GND", (270, 105), (4, 3, 0)))
    P.append(C("C6", "100n", "+3V3", "GND", (270, 115), (-4, 3, 0)))
    # ---- Audio
    T.append((300, 90, "U5  MAX98357A I2S class-D — same pins as d_wip (BCLK 7 / LRC 8 / DIN 44); SD_MODE tied high", 2.0))
    amp = {"~{SD_MODE}": "+3V3", "NC": None, "GAIN_SLOT": "GND", "DIN": "GPIO44_I2S_DIN", "BCLK": "GPIO7_I2S_BCLK", "LRCLK": "GPIO8_I2S_LRC",
           "GND": "GND", "VDD": "+5V", "OUTP": "SPK_P", "OUTN": "SPK_N", "PAD": "GND"}
    P.append(Part("U5", "Audio", "MAX98357A", "MAX98357AETE+T", "Package_DFN_QFN:QFN-16-1EP_3x3mm_P0.5mm_EP1.75x1.75mm",
                  {}, (335, 120), 0, (26, 20, 0), desc="I2S DAC + 3.2W class-D"))
    P.append(C("C7", "10u", "+5V", "GND", (370, 105), (30, 24, 0), fp="Capacitor_SMD:C_0805_2012Metric"))
    P.append(C("C8", "100n", "+5V", "GND", (370, 115), (30, 16, 0)))
    P.append(Part("J6", "Connector", "Conn_01x02_Pin", "SPEAKER 28x40 4Ω", "Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical",
                  {"1": "SPK_P", "2": "SPK_N"}, (400, 118), 0, (34, 30, 0), desc="speaker"))
    # ---- Power: inputs
    T.append((20, 170, "POWER  USB-C 5V  +  dock pogo 5V  -> OR diodes -> BQ24074 1S charger w/ power path -> VSYS ; 1S2P 18650 (2x3000mAh) as ballast", 2.0))
    usb = {"A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS",
           "A5": "USB_CC1", "B5": "USB_CC2", "A6": "USB_DP", "A7": "USB_DN", "B6": "USB_DP", "B7": "USB_DN",
           "A8": None, "B8": None, "S1": "GND"}
    P.append(Part("J1", "Connector", "USB_C_Receptacle_USB2.0_16P", "USB-C service/charge", "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
                  usb, (45, 215), 0, (0, -42.5, 0), desc="USB-C 2.0 16P"))
    P.append(R("R4", "5k1", "USB_CC1", "GND", (80, 200), (-8, -36, 0)))
    P.append(R("R5", "5k1", "USB_CC2", "GND", (80, 210), (8, -36, 0)))
    T.append((60, 232, "USB D+/D- go to the head FPC only if the XIAO USB is exposed there; default: NC (XIAO has its own USB-C for flashing)", 1.5))
    P.append(Part("TP1", "Connector", "TestPoint", "DOCK+ pogo pad Ø3", "TestPoint:TestPoint_Pad_3.0x3.0mm", {"1": "DOCK_VIN"}, (110, 195), 0, (-18, -20, 0), side="B", desc="dock pogo +"))
    P.append(Part("TP2", "Connector", "TestPoint", "DOCK- pogo pad Ø3", "TestPoint:TestPoint_Pad_3.0x3.0mm", {"1": "GND"}, (110, 210), 0, (18, -20, 0), side="B", desc="dock pogo -"))
    P.append(two_pin("D1", "Diode", "PMEG2005EJ", "PMEG2005EJ", "Diode_SMD:D_SOD-323F", "CHG_IN", "VBUS", (140, 195), 0, (-8, -30, 0), desc="OR-ing"))
    P.append(two_pin("D2", "Diode", "PMEG2005EJ", "PMEG2005EJ", "Diode_SMD:D_SOD-323F", "CHG_IN", "DOCK_VIN", (140, 210), 0, (8, -30, 0), desc="OR-ing"))
    P.append(two_pin("F1", "Device", "Polyfuse_Small", "2A", "Fuse:Fuse_1206_3216Metric", "DOCK_VIN", "DOCK_VIN_F", (170, 210), 0, (0, -30, 0)))
    # charger BQ24074: pin names
    chg = {"IN": "CHG_IN", "OUT": "VSYS", "BAT": "VBAT", "VSS": "GND", "~{CE}": "GND", "EN1": "GND", "EN2": "+3V3",
           "ILIM": "CHG_ILIM", "ISET": "CHG_ISET", "ITERM": "CHG_ITERM", "TMR": "CHG_TMR", "TS": "CHG_TS", "~{PGOOD}": "EXP_P5_CHG_PGOOD",
           "~{CHG}": "EXP_P3_CHG_STAT1"}
    P.append(Part("U6", "Battery_Management", "BQ24074RGT", "BQ24074RGT", "Package_DFN_QFN:VQFN-16-1EP_3x3mm_P0.5mm_EP1.68x1.68mm",
                  {}, (215, 210), 0, (-18, -18, 0), desc="1S Li-ion charger + power path"))
    P.append(C("C9", "4u7", "CHG_IN", "GND", (255, 190), (-26, -22, 0), fp="Capacitor_SMD:C_0603_1608Metric"))
    P.append(C("C10", "10u", "VSYS", "GND", (255, 200), (-10, -24, 0), fp="Capacitor_SMD:C_0805_2012Metric"))
    P.append(C("C11", "10u", "VBAT", "GND", (255, 210), (-10, -12, 0), fp="Capacitor_SMD:C_0805_2012Metric"))
    P.append(R("R6", "1k1 (ILIM 1.5A)", "CHG_ILIM", "GND", (255, 220), (-34, -34, 0)))
    P.append(R("R7", "590 (ISET 1.5A)", "CHG_ISET", "GND", (255, 230), (-34, -30, 0)))
    P.append(R("R8", "10k (TS, no NTC)", "CHG_TS", "GND", (285, 190), (-34, -26, 0)))
    P.append(R("R18", "1k5 (ITERM 150mA)", "CHG_ITERM", "GND", (285, 240), (-34, -18, 0)))
    P.append(R("R9", "10k", "CHG_TMR", "GND", (285, 200), (-34, -22, 0)))
    P.append(Part("J2", "Connector", "Conn_01x02_Pin", "BATT 1S2P 18650", "Connector_JST:JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical",
                  {"1": "VBAT", "2": "GND"}, (285, 215), 0, (4, -18, 0), desc="battery pack"))
    # fuel gauge LC709203F
    T.append((300, 170, "U7  LC709203F I2C fuel gauge @0x0B — V3 had NO battery sensing (no free GPIO)", 2.0))
    fg = {"V_{DD}": "VBAT", "V_{SS}": "GND", "SDA": "SDA", "SCL": "SCL", "~{ALARMB}": "FG_ALARM", "T_{SENSE}": None, "T_{SW}": None, "EP": "GND", "TEST": "GND"}
    P.append(Part("U7", "Battery_Management", "LC709203FQH-01TWG", "LC709203FQH-01TWG", "Package_DFN_QFN:WDFN-8-1EP_3x2mm_P0.5mm_EP1.3x1.4mm",
                  {}, (335, 210), 0, (20, -18, 0), desc="fuel gauge"))
    P.append(C("C12", "1u", "VBAT", "GND", (370, 200), (26, -24, 0)))
    # boost 5V
    T.append((20, 255, "U8  MT3608 boost VSYS->5V @2A (servo, XIAO 5V pin, audio, head LED drivers)    U9  AP2112K 3.3V LDO (IMU, expander, sensors)", 2.0))
    boost = {"SW": "BOOST_SW", "GND": "GND", "FB": "BOOST_FB", "EN": "VSYS", "IN": "VSYS", "NC": None}
    P.append(Part("U8", "Regulator_Switching", "MT3608", "MT3608", "Package_TO_SOT_SMD:SOT-23-6", {}, (60, 285), 0, (16, -8, 0), desc="boost"))
    P.append(two_pin("L1", "Device", "L", "4u7 3A", "Inductor_SMD:L_Taiyo-Yuden_NR-40xx", "VSYS", "BOOST_SW", (100, 275), 0, (24, -8, 0)))
    P.append(two_pin("D3", "Diode", "SS14", "SS14", "Diode_SMD:D_SMA", "+5V", "BOOST_SW", (100, 285), 0, (24, -14, 0)))
    P.append(R("R10", "75k", "+5V", "BOOST_FB", (130, 275), (16, -13, 0)))
    P.append(R("R11", "22k", "BOOST_FB", "GND", (130, 285), (16, -15, 0)))
    P.append(C("C14", "22u", "+5V", "GND", (160, 275), (30, -8, 0), fp="Capacitor_SMD:C_1210_3225Metric"))
    P.append(C("C15", "22u", "VSYS", "GND", (160, 285), (10, -8, 0), fp="Capacitor_SMD:C_1210_3225Metric"))
    ldo = {"VIN": "VSYS", "GND": "GND", "EN": "VSYS", "NC": None, "VOUT": "+3V3"}
    P.append(Part("U9", "Regulator_Linear", "AP2112K-3.3", "AP2112K-3.3", "Package_TO_SOT_SMD:SOT-23-5", {}, (215, 285), 0, (-16, 6, 0), desc="3.3V LDO"))
    P.append(C("C16", "1u", "VSYS", "GND", (250, 275), (-30, 18, 0)))
    P.append(C("C17", "1u", "+3V3", "GND", (250, 285), (-12, 8, 0)))
    # ---- Dock sensing + IR homing
    T.append((300, 255, "DOCK  hall switch (dock magnet) + 38kHz IR receiver for beacon homing (GPIO9, freed by moving the laser to the expander)", 2.0))
    hall = {"VDD": "+3V3", "GND": "GND", "OUTPUT": "EXP_P2_DOCK_DET"}
    P.append(Part("U10", "Sensor_Magnetic", "AH1806-W", "AH1806-W", "Package_TO_SOT_SMD:SOT-23", {}, (335, 285), 0, (28, -38, 0), side="B", desc="hall switch"))
    ir = {"OUT": "GPIO9_IR_RX", "GND": "GND", "Vs": "+3V3"}
    P.append(Part("U11", "Interface_Optical", "TSOP38G36", "IRM-H638T (38kHz)", "OptoDevice:Everlight_IRM-H6xxT", {}, (380, 285), 0, (-14, -36, 90), side="B", desc="IR receiver"))
    P.append(C("C18", "100n", "+3V3", "GND", (410, 275), (-14, -30, 0), side="B"))
    # ---- Head FPC + servo + power switch
    T.append((20, 320, "J7  12-pin FPC to HEAD board: 3V3 5V GND SDA SCL SDB LASER_EN SERVO_PWM IMU/EXP INT  |  J5 head tilt servo (power gated by U12)  |  SW1 main power", 2.0))
    fpc = {"1": "GND", "2": "+5V_SERVO", "3": "+5V", "4": "+3V3", "5": "SDA", "6": "SCL", "7": "EXP_P0_LASER_EN", "8": "LED_SDB",
           "9": "GPIO43_SERVO_PWM", "10": "EXP_INT", "11": "GND", "12": "GND", "MP": "GND"}
    P.append(Part("J7", "Connector", "Conn_01x12_Pin", "HEAD FPC 12p", "Connector_FFC-FPC:Hirose_FH12-12S-0.5SH_1x12-1MP_P0.50mm_Horizontal",
                  fpc, (45, 355), 0, (0, 42, 180), desc="head harness"))
    P.append(Part("J5", "Connector", "Conn_01x03_Pin", "SERVO SG90", "Connector_JST:JST_PH_B3B-PH-K_1x03_P2.00mm_Vertical",
                  {"1": "GPIO43_SERVO_PWM", "2": "+5V_SERVO", "3": "GND"}, (100, 350), 0, (-28, 34, 90), desc="head tilt servo"))
    pfet = {"G": "SERVO_GATE", "S": "+5V", "D": "+5V_SERVO"}
    P.append(Part("Q1", "Transistor_FET", "AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23", {}, (150, 350), 0, (-30, 22, 0), desc="servo power P-FET"))
    P.append(R("R12", "10k", "+5V", "SERVO_GATE", (185, 340), (-34, 16, 0)))
    nfet = {"G": "EXP_P1_SERVO_PWR_EN", "S": "GND", "D": "SERVO_GATE"}
    P.append(Part("Q2", "Transistor_FET", "AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23", {}, (150, 375), 0, (-30, 12, 0), desc="level shift"))
    P.append(R("R13", "100k", "EXP_P1_SERVO_PWR_EN", "GND", (185, 375), (-34, 8, 0)))
    P.append(R("R14", "10k", "+3V3", "EXP_INT", (215, 340), (-22, 0, 0)))
    P.append(R("R15", "10k", "+3V3", "IMU_INT1", (215, 350), (-16, 0, 0)))
    P.append(R("R16", "10k", "+3V3", "FG_ALARM", (215, 360), (-10, 0, 0)))
    P.append(Part("SW1", "Switch", "SW_SPDT", "POWER", "Button_Switch_SMD:SW_SPDT_PCM12", {"1": None, "2": "VBAT_CELLS", "3": "VBAT"},
                  (260, 350), 0, (-29, -40, 0), desc="main power slide switch"))
    T.append((240, 362, "SW1 breaks the cell path; VBAT_CELLS goes to the 18650 holder (BT1/BT2 in body, not on PCB). J2 is the pack connector.", 1.5))
    P.append(Part("D4", "Device", "LED", "STATUS (ember)", "LED_SMD:LED_0603_1608Metric", {"1": "GND", "2": "LED_K"}, (300, 340), 0, (30, -40, 0)))
    P.append(R("R17", "1k", "+3V3", "LED_K", (300, 352), (30, -36, 0)))

    # resolve name-based net maps for ICs (nets given by pin NAME not number)
    name_maps = {"U3": tca_by_name(), "U2": drv, "U4": imu, "U5": amp, "U6": chg, "U7": fg, "U8": boost, "U9": ldo,
                 "U10": hall, "U11": ir, "Q1": pfet, "Q2": nfet}
    resolve_by_name(P, name_maps)

    texts = [(20, 10, "ORBIE V4 — MAIN BOARD (body pod).  Pendulum chassis: this board + 1S2P 18650 pack sit BELOW the wheel axle.", 3.0)] + T
    pf = [("+5V", (350, 40)), ("VBUS", (410, 40)), ("GND", (430, 40)), ("DOCK_VIN", (450, 40)), ("+5V_SERVO", (470, 40)),
          ("+3V3_XIAO", (490, 40)), ("VBAT_CELLS", (510, 40)), ("DRV_VINT", (530, 40)), ("CHG_IN", (550, 40))]
    sch = os.path.join(out, name + ".kicad_sch")
    root = write_schematic(sch, name, P, texts, pf, paper="A2", title="Orbie V4 Main Board")
    holes = [(-18.6, 40), (18.6, 40), (-18.6, -40), (18.6, -40)]
    ptexts = [(0, 44, "ORBIE V4 MAIN r0.1", 1.5, "F_SilkS"), (0, -44, "CERN-OHL-S-2.0  orbierobot.com", 1.0, "F_SilkS"),
              (0, 34, "XIAO ESP32-S3 SENSE", 1.0, "F_SilkS"), (0, -20, "DOCK PADS (bottom)", 1.0, "B_SilkS")]
    write_pcb(os.path.join(out, name + ".kicad_pcb"), P, (78, 92, 6), holes, ptexts, "Orbie V4 Main Board")
    write_project(os.path.join(out, name + ".kicad_pro"), name, root)
    return P

def tca_by_name():
    return {"SDA": "SDA", "SCL": "SCL", "~{INT}": "EXP_INT", "P0": "EXP_P0_LASER_EN", "P1": "EXP_P1_SERVO_PWR_EN", "P2": "EXP_P2_DOCK_DET",
            "P3": "EXP_P3_CHG_STAT1", "P4": "EXP_P4_CHG_STAT2", "P5": "EXP_P5_CHG_PGOOD", "P6": "EXP_P6_DRV_nSLEEP", "P7": "EXP_P7_DRV_nFAULT",
            "A0": "GND", "A1": "GND", "A2": "GND", "GND": "GND", "VDD": "+3V3"}

def resolve_by_name(parts, name_maps):
    """Fill part.nets (by pin number) from a {pin_name: net} map."""
    for p in parts:
        if p.ref not in name_maps: continue
        nm = name_maps[p.ref]
        symnode = flatten_symbol(p.lib, p.sym)
        pins = symbol_pins(symnode)
        unmatched = set(nm.keys())
        for num, pname, *_ in pins:
            if pname in nm:
                if nm[pname] is not None: p.nets[num] = nm[pname]
                unmatched.discard(pname)
        if unmatched:
            print(f"  WARNING {p.ref} {p.sym}: pin names not found: {sorted(unmatched)}; available: {[q[1] for q in pins]}")

# ============================================================ HEAD BOARD
def head_board(out):
    name = "orbie_v4_head"
    P = []; T = []
    T.append((20, 18, "HEAD BOARD 80x36 — kept LIGHT: 2x IS31FL3733 (16x8 RGB face), VL53L0X ToF, MLX90614 IR temp (module), laser FET. Camera FPC passes through to the XIAO in the body.", 2.0))
    fpc = {"1": "GND", "2": "+5V_SERVO", "3": "+5V", "4": "+3V3", "5": "SDA", "6": "SCL", "7": "LASER_EN", "8": "LED_SDB",
           "9": "SERVO_PWM", "10": "EXP_INT", "11": "GND", "12": "GND", "MP": "GND"}
    P.append(Part("J1", "Connector", "Conn_01x12_Pin", "BODY FPC 12p", "Connector_FFC-FPC:Hirose_FH12-12S-0.5SH_1x12-1MP_P0.50mm_Horizontal",
                  fpc, (45, 60), 0, (0, -14, 0), desc="body harness"))
    # two LED matrix drivers, addresses via ADDR1/ADDR2
    def isf(ref, pos, pcbpos, a1, a2):
        m = {"SDA": "SDA", "SCL": "SCL", "ADDR1": a1, "ADDR2": a2, "~{SDB}": "LED_SDB", "IICRST": "+3V3", "~{INTB}": None, "SYNC": None, "NC": None,
             "PVCC": "+5V", "AVCC": "+5V", "DVCC": "+5V", "VIO": "+3V3", "GND": "GND", "PGND": "GND", "AGND": "GND", "RSET": f"{ref}_RSET"}
        for i in range(1, 17): m[f"CS{i}"] = f"{ref}_CS{i}"
        for i in range(1, 13): m[f"SW{i}"] = f"{ref}_SW{i}"
        P.append(Part(ref, "Driver_LED", "IS31FL3733-QF", "IS31FL3733-QFLS4", "Package_DFN_QFN:QFN-48-1EP_6x6mm_P0.4mm_EP4.2x4.2mm",
                      {}, pos, 0, pcbpos, desc="12x16 LED matrix driver"))
        return m
    m1 = isf("U1", (120, 80), (-22, 0, 0), "GND", "GND")        # 0x50 (left eye, as V3)
    m2 = isf("U2", (220, 80), (22, 0, 0), "+3V3", "+3V3")      # 0x5F (right eye, as V3)
    P.append(R("R2", "20k (RSET)", "U1_RSET", "GND", (300, 90), (-30, -8, 0)))
    P.append(R("R3", "20k (RSET)", "U2_RSET", "GND", (300, 100), (30, -10, 0)))
    P.append(C("C1", "10u", "+5V", "GND", (300, 40), (-16, 12, 0), fp="Capacitor_SMD:C_0805_2012Metric"))
    P.append(C("C2", "100n", "+5V", "GND", (300, 50), (-10, 12, 0)))
    P.append(C("C3", "10u", "+5V", "GND", (300, 60), (14, 10, 0), fp="Capacitor_SMD:C_0805_2012Metric"))
    P.append(C("C4", "100n", "+5V", "GND", (300, 70), (30, 10, 0)))
    T.append((100, 140, "LED matrix: 2 x (8 rows x 16 cols) RGB 0606 LEDs = 256 LEDs, not drawn — see head_led_matrix.csv for the CS/SW map (same as the V3 display PCB).", 1.5))
    # ToF
    tof = {"AVDDVCSEL": "+3V3", "AVSSVCSEL": "GND", "GND": "GND", "XSHUT": "+3V3", "GPIO1": None, "DNC": None, "SDA": "SDA", "SCL": "SCL", "AVDD": "+3V3"}
    P.append(Part("U3", "Sensor_Distance", "VL53L0CXV0DH1", "VL53L0CXV0DH1", "Sensor_Distance:ST_VL53L1x", {}, (120, 190), 0, (-12, -12, 0), desc="ToF @0x29"))
    P.append(C("C5", "100n", "+3V3", "GND", (160, 180), (-26, -8, 0)))
    P.append(C("C6", "4u7", "+3V3", "GND", (160, 190), (-20, -8, 0), fp="Capacitor_SMD:C_0603_1608Metric"))
    # MLX90614 module header (TO-39 module as in V3)
    P.append(Part("J2", "Connector", "Conn_01x04_Pin", "MLX90614 module", "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
                  {"1": "+3V3", "2": "GND", "3": "SCL", "4": "SDA"}, (220, 190), 0, (12, -8, 0), desc="IR thermometer @0x5A"))
    # laser
    P.append(Part("J3", "Connector", "Conn_01x02_Pin", "KY-008 laser", "Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical",
                  {"1": "+3V3", "2": "LASER_K"}, (300, 190), 0, (24, -13, 0), desc="650nm laser"))
    nfet = {"G": "LASER_EN", "S": "GND", "D": "LASER_K"}
    P.append(Part("Q1", "Transistor_FET", "AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23", {}, (340, 190), 0, (32, -4, 0), desc="laser switch"))
    P.append(R("R1", "100k", "LASER_EN", "GND", (370, 180), (18, -8, 0)))
    P.append(Part("J4", "Connector", "Conn_01x03_Pin", "SERVO passthrough (opt.)", "Connector_JST:JST_PH_B3B-PH-K_1x03_P2.00mm_Vertical",
                  {"1": "SERVO_PWM", "2": "+5V_SERVO", "3": "GND"}, (45, 120), 0, (-26, 10, 0), desc="if servo mounted in head"))
    resolve_by_name(P, {"U1": m1, "U2": m2, "U3": tof, "Q1": nfet})
    texts = [(20, 10, "ORBIE V4 — HEAD BOARD (display + sensors).  Mass target < 60 g incl. shell; the XIAO, speaker and battery move to the body.", 3.0)] + T
    pf = [("+3V3", (330, 120)), ("+5V", (350, 120)), ("GND", (370, 120)), ("+5V_SERVO", (390, 120))]
    root = write_schematic(os.path.join(out, name + ".kicad_sch"), name, P, texts, pf, paper="A3", title="Orbie V4 Head Board")
    ptexts = [(0, 15.5, "ORBIE V4 HEAD r0.1", 1.2, "F_SilkS"), (0, -15.5, "LED MATRIX 16x8 RGB (LEDs not placed)", 0.9, "F_SilkS")]
    write_pcb(os.path.join(out, name + ".kicad_pcb"), P, (80, 36, 4), [(-36, 13), (36, 13), (-36, -13), (36, -13)], ptexts, "Orbie V4 Head Board")
    write_project(os.path.join(out, name + ".kicad_pro"), name, root)
    # LED map csv
    with open(os.path.join(out, "head_led_matrix.csv"), "w") as f:
        f.write("driver,eye,row(SW),col(CS),colour_channel\n")
        for drv, eye in (("U1", "left"), ("U2", "right")):
            for row in range(8):
                for col in range(16):
                    for ch, sw in (("R", 1), ("G", 2), ("B", 3)):
                        f.write(f"{drv},{eye},SW{row*3+sw},CS{col+1},{ch}\n")
    return P

# ============================================================ BOM
def write_bom(out, name, parts):
    with open(os.path.join(out, name + "_bom.csv"), "w") as f:
        f.write("Ref,Value,Symbol,Footprint,Side,Description\n")
        for p in parts:
            f.write(f'{p.ref},"{p.value}",{p.lib}:{p.sym},{p.footprint},{p.side},"{p.desc}"\n')

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    print("== main board"); mp = main_board(out); write_bom(out, "orbie_v4_main", mp)
    print("== head board"); hp = head_board(out); write_bom(out, "orbie_v4_head", hp)
    print("done ->", out)

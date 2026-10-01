#!/usr/bin/env python3
"""
Route + finish a generated board:  DSN export -> freerouting -> SES import -> GND pours -> save.
Then (via kicad-cli) DRC, gerbers + drill + pick&place, and a JLCPCB-style BOM.

  <kicad python> route_v5.py v5/orbie_v5_body.kicad_pcb [--passes 30] [--skip-route]
"""
import os, sys, subprocess, shutil, zipfile, csv, json
import pcbnew
from pcbnew import VECTOR2I, FromMM

CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
FR_CANDIDATES = [os.environ.get("FREEROUTING", ""),
                 "/Applications/Freerouting.app/Contents/MacOS/Freerouting",
                 "/Applications/freerouting.app/Contents/MacOS/freerouting"]

def find_freerouting():
    for c in FR_CANDIDATES:
        if c and os.path.exists(c): return [c]
    for vol in os.listdir("/Volumes"):
        for root, dirs, files in os.walk(os.path.join("/Volumes", vol)):
            for f in files:
                if f.lower().startswith("freerouting") and os.access(os.path.join(root, f), os.X_OK) and "MacOS" in root:
                    return [os.path.join(root, f)]
            if root.count("/") > 6: dirs[:] = []
    return None

def add_gnd_pours(board, solid=False):
    for z in list(board.Zones()):
        board.Remove(z)
    bbox = board.GetBoardEdgesBoundingBox()
    x0, y0, x1, y1 = bbox.GetLeft(), bbox.GetTop(), bbox.GetRight(), bbox.GetBottom()
    gnd = board.FindNet("GND")
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer); z.SetNet(gnd); z.SetIsFilled(False)
        z.SetLocalClearance(FromMM(0.25)); z.SetMinThickness(FromMM(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL if solid else pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetThermalReliefGap(FromMM(0.3)); z.SetThermalReliefSpokeWidth(FromMM(0.4))
        ol = z.Outline(); ol.NewOutline()
        for (x, y) in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            ol.Append(x, y)
        z.SetZoneName(f"GND_{'F' if layer == pcbnew.F_Cu else 'B'}")
        board.Add(z)
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())

def main():
    pcb = os.path.abspath(sys.argv[1]); passes = 30; do_route = "--skip-route" not in sys.argv
    if "--passes" in sys.argv: passes = int(sys.argv[sys.argv.index("--passes") + 1])
    base = pcb[:-len(".kicad_pcb")]; name = os.path.basename(base); outdir = os.path.join(os.path.dirname(pcb), "out"); os.makedirs(outdir, exist_ok=True)
    board = pcbnew.LoadBoard(pcb)
    # design rules for the autorouter / DRC (2-layer JLC standard: 0.15 mm min, we use 0.2/0.2)
    ds = board.GetDesignSettings()
    ds.m_MinClearance = FromMM(0.127); ds.m_TrackMinWidth = FromMM(0.127); ds.m_ViasMinSize = FromMM(0.5); ds.m_MinThroughDrill = FromMM(0.3)
    nc = board.GetAllNetClasses()
    dflt = nc["Default"] if "Default" in nc else None
    if dflt is not None:
        dflt.SetClearance(FromMM(0.15)); dflt.SetTrackWidth(FromMM(0.2)); dflt.SetViaDiameter(FromMM(0.6)); dflt.SetViaDrill(FromMM(0.3))
    # stock footprints (e.g. HTSSOP-16) carry a 0.2 mm local clearance the autorouter never sees -> use the netclass rule everywhere
    for fp in board.GetFootprints():
        fp.SetLocalClearance(0)
        for pad in fp.Pads(): pad.SetLocalClearance(0)
    if do_route:
        dsn = base + ".dsn"; ses = base + ".ses"
        pcbnew.SaveBoard(pcb, board)
        ok = pcbnew.ExportSpecctraDSN(board, dsn); print("DSN export", ok, dsn, flush=True)
        fr = find_freerouting()
        if not fr: raise SystemExit("freerouting not found")
        cmd = fr + ["-de", dsn, "-do", ses, "-mp", str(passes), "-oit", "0.2", "-mt", "1"]
        print("running", " ".join(cmd), flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
        print(r.stdout[-2000:], r.stderr[-1500:], flush=True)
        if not os.path.exists(ses): raise SystemExit("no SES produced")
        board = pcbnew.LoadBoard(pcb)
        ok = pcbnew.ImportSpecctraSES(board, ses); print("SES import", ok, flush=True)
    add_gnd_pours(board, solid="--solid-pads" in sys.argv)
    pcbnew.SaveBoard(pcb, board)
    print("tracks", len([t for t in board.GetTracks()]), flush=True)
    # ---- checks + fab outputs
    subprocess.run([CLI, "pcb", "drc", "--format", "json", "--severity-all", "-o", os.path.join(outdir, name + ".drc.json"), pcb], capture_output=True)
    d = json.load(open(os.path.join(outdir, name + ".drc.json")))
    errs = [v for v in d["violations"] if v["severity"] == "error"]
    import collections
    print("DRC errors", len(errs), dict(collections.Counter(v["type"] for v in errs)), "unrouted", len(d["unconnected_items"]), flush=True)
    gdir = os.path.join(outdir, name + "_gerbers"); shutil.rmtree(gdir, ignore_errors=True); os.makedirs(gdir)
    subprocess.run([CLI, "pcb", "export", "gerbers", "--layers", "F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts",
                    "--subtract-soldermask", "--no-protel-ext", "-o", gdir + "/", pcb], capture_output=True)
    subprocess.run([CLI, "pcb", "export", "drill", "--format", "excellon", "--excellon-units", "mm", "--generate-map", "--map-format", "gerberx2", "-o", gdir + "/", pcb], capture_output=True)
    subprocess.run([CLI, "pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "both", "--use-drill-file-origin", "-o", os.path.join(outdir, name + "_pos.csv"), pcb], capture_output=True)
    z = os.path.join(outdir, name + "_gerbers.zip")
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(os.listdir(gdir)): zf.write(os.path.join(gdir, f), f)
    for side in ("top", "bottom"):
        subprocess.run([CLI, "pcb", "render", "--side", side, "--zoom", "1.1", "-w", "1600", "-h", "1200", "--background", "opaque", "-o", os.path.join(outdir, f"{name}.{side}.png"), pcb], capture_output=True)
    subprocess.run([CLI, "pcb", "export", "svg", "--layers", "F.Cu,B.Cu,Edge.Cuts,F.SilkS", "--page-size-mode", "2", "-o", os.path.join(outdir, name + ".pcb.svg"), pcb], capture_output=True)
    sch = base + ".kicad_sch"
    if os.path.exists(sch):
        subprocess.run([CLI, "sch", "export", "pdf", "-o", os.path.join(outdir, name + ".sch.pdf"), sch], capture_output=True)
    # JLCPCB-style BOM (Comment, Designator, Footprint, LCSC) from the generator BOM
    gen_bom = base + "_bom.csv"
    if os.path.exists(gen_bom):
        rows = list(csv.DictReader(open(gen_bom)))
        groups = {}
        for r in rows:
            if r["Ref"] == "U1B": continue
            if r["Ref"] == "U1A": r = dict(r, Ref="U1", Value="XIAO RP2040 (Seeed 102010428)")
            key = (r["Value"], r["Footprint"].split(":")[-1])
            groups.setdefault(key, []).append(r["Ref"])
        with open(os.path.join(outdir, name + "_bom_jlc.csv"), "w", newline="") as f:
            w = csv.writer(f); w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #", "Qty"])
            for (val, fp), refs in groups.items():
                w.writerow([val, ",".join(refs), fp, "", len(refs)])
    print("outputs in", outdir, flush=True)

if __name__ == "__main__":
    main()

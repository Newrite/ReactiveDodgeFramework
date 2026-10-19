#!/usr/bin/env python3
"""Stage + run Pandora Behaviour Engine Plus OUTSIDE MO2, writing only below _work/.

Why: Pandora is a GUI exe that normally runs through MO2's usvfs and writes into the mod `mods\\Pandora`.
This script rebuilds the same inputs on disk instead:

  _work/pandora_stage/app/              copy of the "Pandora Behaviour Engine Plus" mod (exe + Template + core mods, no .pdb)
                                        + Settings.json that points Pandora at the stage (NOT at the real output mod)
  _work/pandora_stage/game/SkyrimSE.exe empty marker file so Pandora accepts the folder as a game directory
  _work/pandora_stage/game/Data/        overlay of every ENABLED mod of profile STB (modlist.txt: top line = highest priority,
                                        so lower lines are copied first and higher lines overwrite), restricted to the files
                                        Pandora reads: Pandora_Engine/mod, Nemesis_Engine/mod, FNIS_* lists/behaviors,
                                        then MO2's overwrite folder, then (optionally) the RDF mod folder.
  _work/pandora_test_out/<label>/       the Pandora output (ActiveMods.json is seeded from the real one)

Nothing under F:\\...\\mods is written. `--check-real-untouched` hashes mods\\Pandora\\Engine.log + ActiveMods.json before/after.

  python tools/stage_pandora.py --label baseline                      # same inputs as the real run (no rdf)
  python tools/stage_pandora.py --label rdf --rdf behavior/Pandora_Engine/mod/rdf
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MO2 = r"F:\STB\Skyrim\[STB] Mod Organizer"
MODS = os.path.join(MO2, "mods")
PROFILE = os.path.join(MO2, "profiles", "STB", "modlist.txt")
PANDORA_MOD = "Pandora Behaviour Engine Plus"
REAL_OUT = os.path.join(MODS, "Pandora")
STAGE = os.path.join(ROOT, "_work", "pandora_stage")
OUT_ROOT = os.path.join(ROOT, "_work", "pandora_test_out")

FNIS_GLOBS = [  # relative to a mod folder; depth-limited on purpose (walking every mesh folder would take ages)
    "meshes/actors/*/animations/*/FNIS_*",
    "meshes/actors/*/*/animations/*/FNIS_*",
    "meshes/actors/*/_1stperson/animations/*/FNIS_*",
    "meshes/actors/*/behaviors/FNIS_*",
    "meshes/actors/*/*/behaviors/FNIS_*",
    "meshes/actors/*/_1stperson/behaviors/FNIS_*",
]


def enabled_mods_low_to_high():
    names = []
    with open(PROFILE, encoding="utf-8", errors="replace") as f:
        for ln in f:
            ln = ln.rstrip("\n")
            if ln.startswith("+"):
                names.append(ln[1:])
    return list(reversed(names))  # file order is high -> low priority


def copy_tree(src, dst):
    if not os.path.isdir(src):
        return 0
    n = 0
    for dp, _dn, fn in os.walk(src):
        rel = os.path.relpath(dp, src)
        td = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(td, exist_ok=True)
        for f in fn:
            shutil.copyfile(os.path.join(dp, f), os.path.join(td, f))
            n += 1
    return n


def overlay_mod(mod_dir, data):
    n = 0
    n += copy_tree(os.path.join(mod_dir, "Pandora_Engine", "mod"), os.path.join(data, "Pandora_Engine", "mod"))
    n += copy_tree(os.path.join(mod_dir, "Nemesis_Engine", "mod"), os.path.join(data, "Nemesis_Engine", "mod"))
    for pat in FNIS_GLOBS:
        for p in glob.glob(os.path.join(glob.escape(mod_dir), *pat.split("/"))):
            if os.path.isfile(p):
                rel = os.path.relpath(p, mod_dir)
                os.makedirs(os.path.dirname(os.path.join(data, rel)), exist_ok=True)
                shutil.copyfile(p, os.path.join(data, rel))
                n += 1
    return n


def build_stage(rdf_dir, rebuild_base):
    app = os.path.join(STAGE, "app")
    game = os.path.join(STAGE, "game")
    data = os.path.join(game, "Data")
    base_marker = os.path.join(STAGE, ".base_built")
    if rebuild_base or not os.path.exists(base_marker):
        shutil.rmtree(STAGE, ignore_errors=True)
        os.makedirs(data)
        src = os.path.join(MODS, PANDORA_MOD)
        for dp, _dn, fn in os.walk(src):
            rel = os.path.relpath(dp, src)
            if rel == "." or not rel.lower().startswith(("pandora_engine", "nemesis_engine")):
                pass
            for f in fn:
                if f.lower().endswith(".pdb") or f.lower() == "meta.ini":
                    continue
                td = os.path.join(app, rel) if rel != "." else app
                os.makedirs(td, exist_ok=True)
                shutil.copyfile(os.path.join(dp, f), os.path.join(td, f))
        open(os.path.join(game, "SkyrimSE.exe"), "wb").close()
        mods = enabled_mods_low_to_high()
        total = 0
        for i, m in enumerate(mods):
            d = os.path.join(MODS, m)
            if os.path.isdir(d):
                total += overlay_mod(d, data)
        total += overlay_mod(os.path.join(MO2, "overwrite"), data)
        print(f"base overlay: {len(mods)} enabled mods, {total} files")
        open(base_marker, "w").write(time.ctime())
    # RDF layer is always refreshed (cheap) and removed again when not requested
    rdf_target = os.path.join(data, "Pandora_Engine", "mod", "rdf")
    shutil.rmtree(rdf_target, ignore_errors=True)
    if rdf_dir:
        copy_tree(rdf_dir, rdf_target)
    with open(os.path.join(app, "Settings.json"), "w") as f:
        json.dump({"app": {"theme": 0}, "games": {"SkyrimSE": {"gameDataPath": data}}}, f, indent=2)
    return app, data


def sha(p):
    return hashlib.sha1(open(p, "rb").read()).hexdigest() if os.path.exists(p) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="baseline")
    ap.add_argument("--rdf", help="path to a Pandora_Engine/mod/rdf folder to include (and activate)")
    ap.add_argument("--rebuild-base", action="store_true")
    ap.add_argument("--no-run", action="store_true")
    ap.add_argument("--timeout", type=int, default=900)
    a = ap.parse_args()

    guard = [os.path.join(REAL_OUT, "Engine.log"), os.path.join(REAL_OUT, "Pandora_Engine", "ActiveMods.json"),
             os.path.join(REAL_OUT, "meshes", "actors", "character", "Behaviors", "0_Master.hkx")]
    before = [sha(p) for p in guard]

    app, data = build_stage(a.rdf, a.rebuild_base)
    out = os.path.join(OUT_ROOT, a.label)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "Pandora_Engine"))
    # output path is read from Settings.json: set it per label
    with open(os.path.join(app, "Settings.json"), "w") as f:
        json.dump({"app": {"theme": 0}, "games": {"SkyrimSE": {"gameDataPath": data, "outputPath": out}}}, f, indent=2)
    # seed ActiveMods.json from the real one (same on/off + priorities), optionally add rdf
    real_am = os.path.join(REAL_OUT, "Pandora_Engine", "ActiveMods.json")
    am = json.load(open(real_am))
    if a.rdf:
        am.append({"code": "rdf", "active": True, "priority": max(e["priority"] for e in am) + 1})
    json.dump(am, open(os.path.join(out, "Pandora_Engine", "ActiveMods.json"), "w"), indent=2)

    if a.no_run:
        return 0
    exe = os.path.join(app, "Pandora Behaviour Engine+.exe")
    t0 = time.time()
    p = subprocess.Popen([exe, "--auto_run", "--auto_close"], cwd=app)
    try:
        p.wait(timeout=a.timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        print("TIMEOUT - killed")
    print(f"pandora exit code {p.returncode} after {time.time() - t0:.0f}s")
    after = [sha(p_) for p_ in guard]
    print("real output mod untouched:", before == after)
    log = os.path.join(out, "Engine.log")
    print("log:", log, "exists" if os.path.exists(log) else "MISSING")
    return 0


if __name__ == "__main__":
    sys.exit(main())

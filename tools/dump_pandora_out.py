#!/usr/bin/env python3
"""Dump the CURRENT Pandora output (and optionally the vanilla Nemesis-numbered graphs) to XML.

  python tools/dump_pandora_out.py                     # Pandora output -> _work/pandora_out_xml  (hkxc / serde-hkx)
  python tools/dump_pandora_out.py --vanilla           # also vanilla LE cache -> _work/vanilla_nemesis_xml (hkxcmd)
  python tools/dump_pandora_out.py --check             # run hkxcheck on every dumped behavior afterwards

Why two tools / two numberings (verified, see docs/research/behavior-pipeline.md):
  * Pandora patches address nodes by NEMESIS ids (#0340 = RootBehaviorGraph ...).  Those ids come from
    `hkxcmd convert -v:xml` on Nemesis_Engine/cached_behaviors/*.hkx (32-bit LE copies of the vanilla files).
  * serde-hkx (hkxc) numbers objects in file-layout order, so its ids differ from Nemesis ids AND change
    every time Pandora re-serialises the file.  Use hkxc dumps to VERIFY an output by name/class, never to
    pick ids for a patch.
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MODS = r"F:\STB\Skyrim\[STB] Mod Organizer\mods"
HKXC = r"S:\Programming\serde-hkx\target\release\hkxc.exe"
HKXCMD = os.path.join(MODS, "NEMESIS - Unlimited Behavior Engine", "Nemesis_Engine", "hkxcmd.exe")
NEM_CACHE = os.path.join(MODS, "NEMESIS - Unlimited Behavior Engine", "Nemesis_Engine", "cached_behaviors")
PANDORA_OUT = os.path.join(MODS, "Pandora", "meshes", "actors", "character")

# (relative path under PANDORA_OUT, output xml stem)
OUT_FILES = [
    ("Behaviors/0_Master.hkx", "0_Master"),
    ("Behaviors/mt_behavior.hkx", "mt_behavior"),
    ("Behaviors/1hm_behavior.hkx", "1hm_behavior"),
    ("Behaviors/1hm_locomotion.hkx", "1hm_locomotion"),
    ("Behaviors/blockbehavior.hkx", "blockbehavior"),
    ("Behaviors/bashbehavior.hkx", "bashbehavior"),
    ("Behaviors/sprintbehavior.hkx", "sprintbehavior"),
    ("Behaviors/staggerbehavior.hkx", "staggerbehavior"),
    ("Behaviors/idlebehavior.hkx", "idlebehavior"),
    ("Behaviors/weapequip.hkx", "weapequip"),
    ("Behaviors/magicbehavior.hkx", "magicbehavior"),
    ("Behaviors/shout_behavior.hkx", "shout_behavior"),
    ("Characters/DefaultMale.hkx", "DefaultMale"),
    ("Characters Female/DefaultFemale.hkx", "DefaultFemale"),
]
VANILLA = ["0_master", "mt_behavior", "1hm_behavior", "1hm_locomotion", "blockbehavior", "bashbehavior",
           "sprintbehavior", "staggerbehavior", "idlebehavior", "weapequip", "magicbehavior", "shout_behavior",
           "defaultmale", "defaultfemale"]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "_work"))
    ap.add_argument("--vanilla", action="store_true")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    o_dir = os.path.join(a.out, "pandora_out_xml")
    os.makedirs(o_dir, exist_ok=True)
    rc_all = 0
    for rel, stem in OUT_FILES:
        src = os.path.join(PANDORA_OUT, *rel.split("/"))
        if not os.path.exists(src):
            print(f"skip (missing): {src}")
            continue
        dst = os.path.join(o_dir, stem + ".xml")
        rc, out = run([HKXC, "convert", "-i", src, "-o", dst, "-v", "xml"])
        print(("ok   " if rc == 0 and os.path.exists(dst) else "FAIL ") + rel + ("" if rc == 0 else "  " + out[:200]))
        rc_all |= rc
    if a.vanilla:
        v_dir = os.path.join(a.out, "vanilla_nemesis_xml")
        os.makedirs(v_dir, exist_ok=True)
        for stem in VANILLA:
            src = os.path.join(NEM_CACHE, stem + ".hkx")
            tmp = os.path.join(v_dir, stem + ".hkx")
            shutil.copyfile(src, tmp)  # hkxcmd wants a simple relative path; never touch the cache itself
            r = subprocess.run([HKXCMD, "convert", "-v:xml", stem + ".hkx", stem + "_nem.xml"], cwd=v_dir,
                               capture_output=True, text=True)
            os.remove(tmp)
            ok = os.path.exists(os.path.join(v_dir, stem + "_nem.xml"))
            print(("ok   " if ok else "FAIL ") + "vanilla " + stem)
            rc_all |= 0 if ok else 1
    if a.check:
        sys.path.insert(0, HERE)
        import hkxcheck
        char = os.path.join(o_dir, "DefaultMale.xml")
        for rel, stem in OUT_FILES:
            f = os.path.join(o_dir, stem + ".xml")
            if not os.path.exists(f) or stem.startswith("Default"):
                continue
            args = [f, "--max", "5"]
            if os.path.exists(char):
                args += ["--character", char]
            print(f"--- hkxcheck {stem}")
            rc_all |= 1 if hkxcheck.main(args) else 0
    return 1 if rc_all else 0


if __name__ == "__main__":
    sys.exit(main())

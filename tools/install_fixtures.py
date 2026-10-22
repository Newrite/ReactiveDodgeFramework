#!/usr/bin/env python3
r"""Install the RDF behavior patch + LOCAL DEV animation fixtures into the MO2 mod folder.

  python tools/install_fixtures.py [--no-anims] [--mod-dir "F:\...\mods\AI - Reactive Dodge Framework"]

Writes ONLY inside the RDF mod folder (default: mods\AI - Reactive Dodge Framework):
  Pandora_Engine\mod\rdf\...                         <- behavior/Pandora_Engine/mod/rdf (run tools/gen_rdf_patch.py first)
  meshes\actors\character\animations\RDF\rdf_*.hkx   <- 18 clips COPIED from the user's own "The Ultimate Dodge Mod"

The clips belong to TUDM's author. They are development placeholders: never commit them (they are gitignored),
never ship them. The shipped framework needs its own animation set (see ANIMATION_CONTRACT.md).
It does NOT touch modlist.txt or Pandora's ActiveMods.json: enabling the mod / ticking `rdf` is a manual step.
"""
import argparse
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS = r"F:\STB\Skyrim\[STB] Mod Organizer\mods"
SRC_ANIMS = os.path.join(MODS, "The Ultimate Dodge Mod", "meshes", "actors", "character", "animations")
DEFAULT_DEST = os.path.join(MODS, "AI - Reactive Dodge Framework")
# index order must match tools/gen_rdf_patch.py DIRS: f fr r br b bl l fl
SRC_SUFFIX = {"f": "forward", "fr": "forwardright", "r": "right", "br": "backwardright", "b": "backward",
              "bl": "backwardleft", "l": "left", "fl": "forwardleft",
              "n": "backward"}  # neutral (no move input): the backward clip until a set provides its own
KINDS = {"roll": "roll", "step": "sidestep"}
# TUDM's own roll_backwardright.hkx is a byte copy of its roll_forwardright.hkx (and sidestep_forwardright.hkx): a
# back-right roll played the forward-right animation. Its back-right sidestep is a real one, so it fills both slots.
SOURCE_OVERRIDE = {("roll", "br"): "sidestep_backwardright"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mod-dir", default=DEFAULT_DEST)
    ap.add_argument("--no-anims", action="store_true")
    a = ap.parse_args()
    patch_src = os.path.join(ROOT, "behavior", "Pandora_Engine", "mod", "rdf")
    if not os.path.isfile(os.path.join(patch_src, "info.xml")):
        print("run tools/gen_rdf_patch.py first")
        return 1
    dest_patch = os.path.join(a.mod_dir, "Pandora_Engine", "mod", "rdf")
    shutil.rmtree(dest_patch, ignore_errors=True)
    shutil.copytree(patch_src, dest_patch)
    print("patch ->", dest_patch)
    if not a.no_anims:
        dest = os.path.join(a.mod_dir, "meshes", "actors", "character", "animations", "RDF")
        os.makedirs(dest, exist_ok=True)
        n = 0
        for kind, tudm in KINDS.items():
            for d, suf in SRC_SUFFIX.items():
                src = os.path.join(SRC_ANIMS, SOURCE_OVERRIDE.get((kind, d), "%s_%s" % (tudm, suf)) + ".hkx")
                if not os.path.isfile(src):
                    print("MISSING", src)
                    return 1
                shutil.copyfile(src, os.path.join(dest, "rdf_%s_%s.hkx" % (kind, d)))
                n += 1
        print("%d placeholder clips -> %s" % (n, dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())

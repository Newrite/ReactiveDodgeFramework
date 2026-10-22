#!/usr/bin/env python3
r"""Install the RDF behavior patch + LOCAL DEV animation fixtures into the MO2 mod folder.

  python tools/install_fixtures.py [--no-anims] [--mod-dir "F:\...\mods\AI - Reactive Dodge Framework"]

Writes ONLY inside the RDF mod folder (default: mods\AI - Reactive Dodge Framework):
  Pandora_Engine\mod\rdf\...                         <- behavior/Pandora_Engine/mod/rdf (run tools/gen_rdf_patch.py first)
  meshes\actors\character\animations\RDF\RDF_Dodge_*.hkx <- 9 clips (its sidesteps) COPIED from the user's own
                                                        "The Ultimate Dodge Mod"; earlier rdf_roll_* / rdf_step_* removed

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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_rdf_patch as G  # noqa: E402

# TUDM's sidesteps, one per RDF clip (gen_rdf_patch CLIP_DIRS). Its rolls are not used: roll_backwardright.hkx is even a
# byte copy of roll_forwardright.hkx.
SRC_SUFFIX = {"f": "forward", "fr": "forwardright", "r": "right", "br": "backwardright", "b": "backward",
              "bl": "backwardleft", "l": "left", "fl": "forwardleft",
              "n": "backward"}  # neutral (no move input): the backward clip until a set provides its own


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
        for old in os.listdir(dest):   # the earlier roll / step clips (rdf_roll_*, rdf_step_*): ours, replaced
            if old.lower().startswith("rdf_") and old.lower().endswith(".hkx") and not old.startswith("RDF_Dodge_"):
                os.remove(os.path.join(dest, old))
        n = 0
        for d, name in zip(G.CLIP_DIRS, G.CLIPS):
            src = os.path.join(SRC_ANIMS, "sidestep_%s.hkx" % SRC_SUFFIX[d])
            if not os.path.isfile(src):
                print("MISSING", src)
                return 1
            shutil.copyfile(src, os.path.join(dest, name + ".hkx"))
            n += 1
        print("%d placeholder clips -> %s" % (n, dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())

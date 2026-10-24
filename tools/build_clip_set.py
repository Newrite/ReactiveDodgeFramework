"""Build an MO2 mod that swaps RDF's dodge clips for another set through Open Animation Replacer.

RDF's graph always plays meshes\\actors\\character\\animations\\RDF\\RDF_Dodge_<Direction>.hkx (9 clips: 8 directions
and Neutral). A clip set is an OAR replacer mod whose submod mirrors those paths and gates them with conditions (weapon
type...), so sets are added without touching the Pandora patch. The spec (behavior/sets/*.json) names one source clip
per direction (f fr r br b bl l fl, optional n = neutral, else the backward one); it is copied in, then annotated with the RDF_*
windows (tools/annotate_clips.py, the source clip's own annotations are kept, except those starting with one of the
spec's "dropAnnotations" prefixes: another dodge framework's events, such as DMCO's stamina payload, its
SpecialFrames i-frames or MCO's recovery / input-buffer events, which RDF would take for an attack's). Each clip also
gets RDF_Motion / RDF_ClipLength from its root motion (its own distance, phases and length); the spec's optional
"motion": false leaves them out, "distanceScale" multiplies the measured distance, and "speed" adds RDF_Speed (the
set's own fAnimSpeed). "clipSettings" ({"exitBlend": ..., "armStance": ..., "steer": ...}) and "jumpOpen" (seconds or
"exit") add the other per-clip settings, the same for every clip of the set (ANIMATION_CONTRACT.md 5).

    python tools/build_clip_set.py behavior/sets/greatsword-2hm.json --src <extracted archive folder>
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import annotate_clips  # noqa: E402
import gen_rdf_patch as G  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MODS = Path(r"F:\STB\Skyrim\[STB] Mod Organizer\mods")
DIRS = G.DIRS
ANIMATIONS = Path("meshes/actors/character/animations")


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=4, ensure_ascii=False) + "\n", encoding="utf-8")


def build(spec_path, src, mods):
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    missing = [d for d in DIRS if d not in spec["clips"]]
    if missing:
        sys.exit(f"spec has no clip for: {missing}")

    oar = mods / spec["mod"] / ANIMATIONS / "OpenAnimationReplacer" / spec["oarMod"]
    submod = oar / spec["submod"]["name"]
    clips = submod / "RDF"  # mirrors animations\RDF\RDF_Dodge_*.hkx
    if submod.exists():
        shutil.rmtree(submod)
    clips.mkdir(parents=True)

    write_json(oar / "config.json", {"name": spec["oarMod"], "author": spec["author"], "description": spec["description"]})
    write_json(submod / "config.json", {
        "name": spec["submod"]["name"],
        "description": "Replaces RDF's 9 dodge clips (8 directions and the neutral one).",
        "priority": spec["submod"]["priority"],
        "conditions": spec["submod"]["conditions"],
    })

    # "n" (neutral, no move input) is optional in a spec: without it the backward clip fills the slot.
    for d, name in zip(G.CLIP_DIRS, G.CLIPS):
        source = src / spec["clips"].get(d, spec["clips"]["b"])
        if not source.is_file():
            sys.exit(f"missing source clip {source}")
        shutil.copyfile(source, clips / f"{name}.hkx")

    annotate_clips.WORK.mkdir(parents=True, exist_ok=True)
    with_motion = spec.get("motion", True)
    distance_scale = spec.get("distanceScale", 1.0)
    report = {"rule": {"iframeOn": annotate_clips.IFRAME_ON, "actionLatest": annotate_clips.ACTION_LATEST,
                       **annotate_clips.DEFAULT_RULE}, "clips": {}}
    if with_motion:
        report["motionRule"] = {"plateau": annotate_clips.PLATEAU, "distanceScale": distance_scale}
    for clip in sorted(clips.glob("*.hkx")):
        marks, problem = annotate_clips.annotate(clip, annotate_clips.DEFAULT_RULE, spec.get("dropAnnotations", []),
                                                 with_motion, distance_scale, spec.get("speed"),
                                                 spec.get("clipSettings"), spec.get("jumpOpen"))
        if problem:
            sys.exit(f"{clip.name}: {problem}")
        report["clips"][clip.stem] = marks
    out = spec_path.with_suffix(".windows.json")
    write_json(out, report)
    return submod, report, out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("spec", type=Path)
    ap.add_argument("--src", type=Path, required=True, help="folder the source archive was extracted into")
    ap.add_argument("--mods", type=Path, default=MODS)
    args = ap.parse_args(argv)

    submod, report, out = build(args.spec.resolve(), args.src.resolve(), args.mods)
    print(f"built {submod}")
    for name, m in report["clips"].items():
        if name in G.CLIP_DIR_OF:
            print(f"  {G.CLIP_DIR_OF[name]:>2}: peak {m['peakSpeed']:.0f}@{m['peakTime']:.3f}  iframes {m['RDF_IFrameStart']:.3f}-"
                  f"{m['RDF_IFrameEnd']:.3f}  attack/block {m['RDF_AttackOpen']:.3f}  chain {m['RDF_ChainOpen']:.3f}"
                  f"  exit {m['RDF_ExitOpen']:.3f}{annotate_clips.describe_motion(m)}")
    print(f"windows written to {out}")


if __name__ == "__main__":
    main(sys.argv[1:])

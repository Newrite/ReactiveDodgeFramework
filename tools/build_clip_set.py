"""Build an MO2 mod that swaps RDF's dodge clips for another set through Open Animation Replacer.

RDF's graph always plays meshes\\actors\\character\\animations\\RDF\\rdf_{roll,step}_<dir>.hkx. A clip set is
an OAR replacer mod whose submod mirrors those 16 paths and gates them with conditions (weapon type...),
so sets are added without touching the Pandora patch. The spec (behavior/sets/*.json) names one source
clip per direction; it is copied into both the roll and the step slot, then annotated with the RDF_*
windows (tools/annotate_clips.py, the source clip's own annotations are kept).

    python tools/build_clip_set.py behavior/sets/greatsword-2hm.json --src <extracted archive folder>
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import annotate_clips  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MODS = Path(r"F:\STB\Skyrim\[STB] Mod Organizer\mods")
DIRS = ["f", "fr", "r", "br", "b", "bl", "l", "fl"]
SLOTS = ["roll", "step"]
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
    clips = submod / "RDF"  # mirrors animations\RDF\rdf_*.hkx
    if submod.exists():
        shutil.rmtree(submod)
    clips.mkdir(parents=True)

    write_json(oar / "config.json", {"name": spec["oarMod"], "author": spec["author"], "description": spec["description"]})
    write_json(submod / "config.json", {
        "name": spec["submod"]["name"],
        "description": "Replaces RDF's 16 dodge clips (roll and step slots share one clip per direction).",
        "priority": spec["submod"]["priority"],
        "conditions": spec["submod"]["conditions"],
    })

    for d in DIRS:
        source = src / spec["clips"][d]
        if not source.is_file():
            sys.exit(f"missing source clip {source}")
        for slot in SLOTS:
            shutil.copyfile(source, clips / f"rdf_{slot}_{d}.hkx")

    annotate_clips.WORK.mkdir(parents=True, exist_ok=True)
    report = {"rule": {"iframeOn": annotate_clips.IFRAME_ON, "actionLatest": annotate_clips.ACTION_LATEST,
                       **annotate_clips.DEFAULT_RULE}, "clips": {}}
    for clip in sorted(clips.glob("*.hkx")):
        marks, problem = annotate_clips.annotate(clip, annotate_clips.DEFAULT_RULE)
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
        if name.startswith("rdf_roll_"):
            print(f"  {name[9:]:>2}: peak {m['peakSpeed']:.0f}@{m['peakTime']:.3f}  iframes {m['RDF_IFrameStart']:.3f}-"
                  f"{m['RDF_IFrameEnd']:.3f}  attack/block {m['RDF_AttackOpen']:.3f}  chain {m['RDF_ChainOpen']:.3f}"
                  f"  exit {m['RDF_ExitOpen']:.3f}")
    print(f"windows written to {out}")


if __name__ == "__main__":
    main(sys.argv[1:])

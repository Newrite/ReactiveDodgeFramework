"""Write RDF_* window annotations into dodge clips (serde-hkx hkx -> xml -> hkx).

The window times come from each clip's own root-motion speed curve (its `animmotion` keys), so
every direction gets windows that match how that particular animation moves (speeds are measured
across +-2 keys, so a single jerky key cannot become the peak):

    RDF_Windows      0                         the clip carries windows (switches the plugin from ini timers)
    RDF_IFrameStart  speed first >= 30 % of the peak
    RDF_IFrameEnd    after the peak, speed <= 60 % of the peak     (--iframeEnd)
    RDF_AttackOpen   after the peak, speed <= 90 %                  (--attack)
    RDF_BlockOpen    after the peak, speed <= 90 %                  (--block)
                     attack and block: never later than ACTION_LATEST (0.35 s)
    RDF_ChainOpen    after the peak, speed <= 75 %                  (--chain)
    RDF_ExitOpen     after the peak, speed <= 75 % (still moving: blends into running)  (--exit)

Existing RDF_* annotations are replaced, so re-running is idempotent; other annotations
(animmotion, PIE, ...) are kept. A clip whose root motion is missing is reported and skipped.
Times are clip-local seconds. Writes behavior/clip_windows.json as a record of what was written.

    python tools/annotate_clips.py [--dir <animations\\RDF>] [--exit 0.75] [--action 0.6]
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HKXC = Path(r"S:\Programming\serde-hkx\target\release\hkxc.exe")
DEFAULT_DIR = Path(r"F:\STB\Skyrim\[STB] Mod Organizer\mods\AI - Reactive Dodge Framework"
                   r"\meshes\actors\character\animations\RDF")
WORK = ROOT / "_work" / "annotate"
REPORT = ROOT / "behavior" / "clip_windows.json"

IFRAME_ON = 0.30

TRACK0 = re.compile(r'(<hkparam name="annotationTracks"[^>]*>\s*<hkobject>\s*<hkparam name="trackName">[^<]*</hkparam>\s*)'
                    r'<hkparam name="annotations" numelements="(\d+)">(.*?)\n\t{5}</hkparam>(\s*</hkobject>)', re.S)
NOTE = re.compile(r'<hkobject>\s*<hkparam name="time">([^<]*)</hkparam>\s*<hkparam name="text">([^<]*)</hkparam>\s*</hkobject>', re.S)
INDENT = "\t\t\t\t\t\t"


def hkxc(src, dst, fmt):
    result = subprocess.run([str(HKXC), "convert", "-i", str(src), "-o", str(dst), "-v", fmt],
                            capture_output=True, text=True)
    if result.returncode != 0 or not Path(dst).exists():
        sys.exit(f"hkxc failed on {src}: {result.stdout}{result.stderr}")


def speed_curve(notes, span=2):
    """Root-motion speed per key, measured across +-span keys so one jerky key cannot become the peak."""
    keys = [(0.0, 0.0, 0.0)] + [(t, *map(float, s.split()[1:3])) for t, s in notes if s.startswith("animmotion ")]
    curve = []
    for i in range(1, len(keys)):
        lo, hi = keys[max(0, i - span)], keys[min(len(keys) - 1, i + span)]
        if hi[0] > lo[0]:
            curve.append((keys[i][0], ((hi[1] - lo[1]) ** 2 + (hi[2] - lo[2]) ** 2) ** 0.5 / (hi[0] - lo[0])))
    return curve


# Each window opens at the first key after the speed peak where the speed has fallen to this fraction of the peak.
# Attack and block open right after the peak so the dodge can be cut into an action early; a chain waits a little
# longer so the roll reads; i-frames end once most of the displacement is done.
DEFAULT_RULE = {"iframeEnd": 0.60, "chain": 0.75, "attack": 0.90, "block": 0.90, "exit": 0.75}
# Attack and block open no later than this clip time (s) even when the clip peaks late (the forward roll
# peaks at 0.40 s): waiting longer reads as "the dodge has to finish first".
ACTION_LATEST = 0.35


def windows(curve, rule, latest=ACTION_LATEST):
    peak_t, peak = max(curve, key=lambda p: p[1])
    after = [p for p in curve if p[0] > peak_t]

    def falls_to(fraction):
        return next((t for t, v in after if v <= fraction * peak), curve[-1][0])

    return {
        "RDF_Windows": 0.0,
        "RDF_IFrameStart": next(t for t, v in curve if v >= IFRAME_ON * peak),
        "RDF_IFrameEnd": falls_to(rule["iframeEnd"]),
        "RDF_ChainOpen": falls_to(rule["chain"]),
        "RDF_AttackOpen": min(falls_to(rule["attack"]), latest),
        "RDF_BlockOpen": min(falls_to(rule["block"]), latest),
        "RDF_ExitOpen": falls_to(rule["exit"]),
    }, peak_t, peak


def render(notes):
    return "".join(f"\n{INDENT}<hkobject>\n{INDENT}\t<hkparam name=\"time\">{t:.6f}</hkparam>\n"
                   f"{INDENT}\t<hkparam name=\"text\">{s}</hkparam>\n{INDENT}</hkobject>" for t, s in notes)


def annotate(clip, rule, drop=()):
    """drop: prefixes of the clip's own annotations to remove (another framework's events, see build_clip_set)."""
    xml = WORK / (clip.stem + ".xml")
    hkxc(clip, xml, "xml")
    text = xml.read_text(encoding="utf-8")
    m = TRACK0.search(text)
    if not m:
        return None, "no annotation track"
    notes = [(float(t), s) for t, s in NOTE.findall(m.group(3))]
    curve = speed_curve(notes)
    if not curve:
        return None, "no animmotion root motion"

    marks, peak_t, peak = windows(curve, rule)
    kept = [(t, s) for t, s in notes if not s.startswith(("RDF_",) + tuple(drop))]
    merged = sorted(kept + [(t, s) for s, t in marks.items()], key=lambda n: n[0])
    body = f'<hkparam name="annotations" numelements="{len(merged)}">{render(merged)}\n\t\t\t\t\t</hkparam>'
    text = text[:m.start()] + m.group(1) + body + m.group(4) + text[m.end():]
    xml.write_text(text, encoding="utf-8")

    hkxc(xml, clip, "amd64")
    check = WORK / (clip.stem + ".check.xml")
    hkxc(clip, check, "xml")
    found = {s for _, s in NOTE.findall(TRACK0.search(check.read_text(encoding="utf-8")).group(3))}
    missing = [s for s in marks if s not in found]
    if missing:
        sys.exit(f"{clip.name}: annotations missing after repack: {missing}")
    leftover = [s for s in found if drop and s.startswith(tuple(drop))]
    if leftover:
        sys.exit(f"{clip.name}: dropped annotations still there after repack: {leftover}")
    return {"peakTime": round(peak_t, 4), "peakSpeed": round(peak, 1),
            **{k: round(v, 4) for k, v in marks.items()}}, None


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, default=DEFAULT_DIR)
    for key, value in DEFAULT_RULE.items():
        ap.add_argument("--" + key, type=float, default=value, help="speed fraction after the peak that opens this window")
    args = ap.parse_args(argv)

    WORK.mkdir(parents=True, exist_ok=True)
    rule = {key: getattr(args, key) for key in DEFAULT_RULE}
    report = {"rule": {"iframeOn": IFRAME_ON, **rule}, "clips": {}}
    for clip in sorted(args.dir.glob("*.hkx")):
        marks, problem = annotate(clip, rule)
        if problem:
            print(f"skip {clip.name}: {problem}")
            continue
        report["clips"][clip.stem] = marks
        print(f"{clip.name}: peak {marks['peakSpeed']:.0f}@{marks['peakTime']:.3f}  iframes "
              f"{marks['RDF_IFrameStart']:.3f}-{marks['RDF_IFrameEnd']:.3f}  attack/block {marks['RDF_AttackOpen']:.3f}  chain {marks['RDF_ChainOpen']:.3f}"
              f"  exit {marks['RDF_ExitOpen']:.3f}")
    REPORT.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"wrote {REPORT}")


if __name__ == "__main__":
    main(sys.argv[1:])

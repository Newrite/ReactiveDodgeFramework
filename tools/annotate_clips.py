"""Write RDF_* window annotations into dodge clips (serde-hkx hkx -> xml -> hkx).

The window times come from each clip's own root-motion speed curve (its `animmotion` keys), so
every direction gets windows that match how that particular animation moves (speeds are measured
across +-2 keys, so a single jerky key cannot become the peak):

    RDF_Windows      0                         the clip carries cancel windows (protection retains INI duration)
    RDF_IFrameStart.<seconds> at frame 0: start when speed first >= 30 % of peak
    RDF_IFrameEnd    after the peak, speed <= 60 % of the peak     (--iframeEnd)
    RDF_AttackOpen   after the peak, speed <= 90 %                  (--attack)
    RDF_BlockOpen    after the peak, speed <= 90 %                  (--block)
                     attack and block: never later than ACTION_LATEST (0.35 s)
    RDF_ChainOpen    after the peak, speed <= 75 %                  (--chain)
    RDF_ExitOpen     after the peak, speed <= 75 % (still moving: blends into running)  (--exit)

and, unless --no-motion, how the clip moves (both at 0; the plugin replaces fDistance, the [Timing] phases and
fClipLength with them for this clip's dodge, ANIMATION_CONTRACT.md 5):

    RDF_Motion.<distance>,<startup>,<active>,<recovery>
                     distance: the root motion's horizontal path length (x --distanceScale)
                     startup:  until the speed first reaches 85 % of the peak            (--plateau)
                     active:   from there until it falls below 85 % again after the peak
                     recovery: the rest of the clip, so the phases add up to its length
    RDF_ClipLength.<seconds>   the clip's duration (a clip still streaming in reports 0 to the game)
    RDF_Speed.<multiplier>     only with --speed: the clip's own fAnimSpeed (playback x this, the timeline unchanged)

and, when given (the same for every clip of the run; not measured): --exitBlend "<run>,<block>,<attack>" (RDF_ExitBlend),
--armStance <share> (RDF_ArmStance), --steer "<startup>,<active>,<recovery>[,<maxDeviation>]" (RDF_Steer), --warp <deg>
(RDF_Warp), --runSpeed <u/s> (RDF_RunSpeed), --yawRate "<face>[,<startup>]" (RDF_YawRate), --commit <s> (RDF_Commit), and
--jumpOpen <seconds> or "exit" (RDF_JumpOpen at that time, or at the clip's RDF_ExitOpen).

Existing RDF_* annotations are replaced, so re-running is idempotent; other annotations
(animmotion, PIE, ...) are kept. A clip whose root motion is missing is reported and skipped.
Times are clip-local seconds. Writes behavior/clip_windows.json as a record of what was written.

    python tools/annotate_clips.py [--dir <animations\\RDF>] [--exit 0.75] [--action 0.6] [--no-motion] [--distanceScale 1]
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
DURATION = re.compile(r'<hkparam name="duration">([^<]*)</hkparam>')
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


# The plugin's speed curve is a smoothstep up (startup), a plateau (active) and a smoothstep down (recovery); the phases
# are read off the clip's own curve at this share of its peak.
PLATEAU = 0.85
MIN_RECOVERY = 0.05


def path_length(notes):
    """Horizontal length of the root-motion path (animmotion keys are cumulative x y z from the clip's start)."""
    keys = [(0.0, 0.0)] + [tuple(map(float, s.split()[1:3])) for _, s in notes if s.startswith("animmotion ")]
    return sum(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 for a, b in zip(keys, keys[1:]))


def motion(curve, duration, distance, plateau=PLATEAU):
    """(startup, active, recovery) adding up to the clip's duration."""
    peak_t, peak = max(curve, key=lambda p: p[1])
    startup = min(next(t for t, v in curve if v >= plateau * peak), peak_t)
    active_end = next((t for t, v in curve if t > peak_t and v < plateau * peak), peak_t)
    active_end = min(max(active_end, startup), duration - MIN_RECOVERY)
    startup = min(startup, active_end)
    return {"distance": distance, "startup": startup, "active": active_end - startup, "recovery": duration - active_end}


def render(notes):
    return "".join(f"\n{INDENT}<hkobject>\n{INDENT}\t<hkparam name=\"time\">{t:.6f}</hkparam>\n"
                   f"{INDENT}\t<hkparam name=\"text\">{s}</hkparam>\n{INDENT}</hkobject>" for t, s in notes)


# Constant per-clip settings: option -> annotation name (the value is written after the '.').
SETTINGS = {"exitBlend": "RDF_ExitBlend", "armStance": "RDF_ArmStance", "steer": "RDF_Steer", "warp": "RDF_Warp",
            "runSpeed": "RDF_RunSpeed", "yawRate": "RDF_YawRate", "commit": "RDF_Commit"}


def annotate(clip, rule, drop=(), with_motion=True, distance_scale=1.0, speed=None, settings=None, jump_open=None):
    """drop: prefixes of the clip's own annotations to remove (another framework's events, see build_clip_set).
    with_motion: also write RDF_Motion / RDF_ClipLength; distance_scale multiplies the measured distance.
    speed: write RDF_Speed.<speed> (the clip's own fAnimSpeed); None = none.
    settings: {"exitBlend": "0.2,0.15,0", "armStance": 0, "steer": "900,260,600,90"} (any subset) written as they are.
    jump_open: RDF_JumpOpen at this clip time, or "exit" = at the clip's RDF_ExitOpen; None = none."""
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
    extra = [(0.0, f"RDF_IFrameStart.{marks['RDF_IFrameStart']:.6f}")]  # start metadata on frame zero
    moves = None
    if with_motion:
        found = DURATION.search(text)
        duration = float(found.group(1)) if found else 0.0
        if duration <= MIN_RECOVERY:
            return None, "no duration"
        moves = motion(curve, duration, path_length(notes) * distance_scale)
        extra += [(0.0, "RDF_Motion.{distance:.1f},{startup:.3f},{active:.3f},{recovery:.3f}".format(**moves)),
                 (0.0, f"RDF_ClipLength.{duration:.4f}")]
    if speed is not None:
        if not 0.1 <= speed <= 5.0:
            sys.exit(f"--speed {speed}: the plugin takes 0.1 .. 5")
        extra.append((0.0, f"RDF_Speed.{speed:g}"))
    for key, value in (settings or {}).items():
        if key not in SETTINGS:
            sys.exit(f"unknown clip setting '{key}' (known: {', '.join(SETTINGS)})")
        numbers = str(value).replace(" ", "")
        if not re.fullmatch(r"[0-9.]+(,[0-9.]+)*", numbers):
            sys.exit(f"{key} '{value}': numbers separated by commas")
        extra.append((0.0, f"{SETTINGS[key]}.{numbers}"))
    if jump_open is not None:
        extra.append((marks["RDF_ExitOpen"] if jump_open == "exit" else float(jump_open), "RDF_JumpOpen"))
    kept = [(t, s) for t, s in notes if not s.startswith(("RDF_",) + tuple(drop))]
    merged = sorted(kept + [(t, s) for s, t in marks.items() if s != "RDF_IFrameStart"] + extra, key=lambda n: n[0])
    body = f'<hkparam name="annotations" numelements="{len(merged)}">{render(merged)}\n\t\t\t\t\t</hkparam>'
    text = text[:m.start()] + m.group(1) + body + m.group(4) + text[m.end():]
    xml.write_text(text, encoding="utf-8")

    hkxc(xml, clip, "amd64")
    check = WORK / (clip.stem + ".check.xml")
    hkxc(clip, check, "xml")
    found = {s for _, s in NOTE.findall(TRACK0.search(check.read_text(encoding="utf-8")).group(3))}
    missing = [s for s in marks if s != "RDF_IFrameStart" and s not in found] + [s for _, s in extra if s not in found]
    if missing:
        sys.exit(f"{clip.name}: annotations missing after repack: {missing}")
    leftover = [s for s in found if drop and s.startswith(tuple(drop))]
    if leftover:
        sys.exit(f"{clip.name}: dropped annotations still there after repack: {leftover}")
    result = {"protectionContract": "start-duration-v2", "peakTime": round(peak_t, 4), "peakSpeed": round(peak, 1), **{k: round(v, 4) for k, v in marks.items()}}
    if moves:
        result["motion"] = {k: round(v, 4) for k, v in moves.items()}
        result["clipLength"] = round(duration, 4)
    if speed is not None:
        result["speed"] = speed
    if settings:
        result["settings"] = dict(settings)
    if jump_open is not None:
        result["RDF_JumpOpen"] = round(marks["RDF_ExitOpen"] if jump_open == "exit" else float(jump_open), 4)
    return result, None


def describe_motion(marks):
    m = marks.get("motion")
    if not m:
        return ""
    return (f"  motion {m['distance']:.0f} u {m['startup']:.3f}/{m['active']:.3f}/{m['recovery']:.3f}"
            f"  length {marks['clipLength']:.3f}" + (f"  speed x{marks['speed']:g}" if "speed" in marks else ""))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path, default=DEFAULT_DIR)
    for key, value in DEFAULT_RULE.items():
        ap.add_argument("--" + key, type=float, default=value, help="speed fraction after the peak that opens this window")
    ap.add_argument("--no-motion", action="store_true", help="do not write RDF_Motion / RDF_ClipLength")
    ap.add_argument("--distanceScale", type=float, default=1.0, help="multiplies the measured root-motion distance")
    ap.add_argument("--speed", type=float, default=None, help="write RDF_Speed.<x>: the clips' own fAnimSpeed")
    ap.add_argument("--exitBlend", default=None, help='RDF_ExitBlend: "<run>,<block>,<attack>" or one value')
    ap.add_argument("--armStance", default=None, help="RDF_ArmStance: the stance share 0..1")
    ap.add_argument("--steer", default=None, help='RDF_Steer: "<startup>,<active>,<recovery>[,<maxDeviation>]"')
    ap.add_argument("--warp", default=None, help="RDF_Warp: the clips' own fOrientationWarpMax (deg)")
    ap.add_argument("--runSpeed", default=None, help="RDF_RunSpeed: the clips' own fRunSpeed")
    ap.add_argument("--yawRate", default=None, help='RDF_YawRate: "<face>[,<startup>]" deg/s')
    ap.add_argument("--commit", default=None, help="RDF_Commit: the clips' own fDirectionCommitTime (s)")
    ap.add_argument("--jumpOpen", default=None, help='RDF_JumpOpen at this clip time, or "exit" (= RDF_ExitOpen)')
    args = ap.parse_args(argv)

    WORK.mkdir(parents=True, exist_ok=True)
    rule = {key: getattr(args, key) for key in DEFAULT_RULE}
    report = {"rule": {"iframeOn": IFRAME_ON, **rule}, "clips": {}}
    if not args.no_motion:
        report["motionRule"] = {"plateau": PLATEAU, "distanceScale": args.distanceScale}
    for clip in sorted(args.dir.glob("*.hkx")):
        settings = {k: getattr(args, k) for k in SETTINGS if getattr(args, k) is not None}
        marks, problem = annotate(clip, rule, with_motion=not args.no_motion, distance_scale=args.distanceScale,
                                  speed=args.speed, settings=settings, jump_open=args.jumpOpen)
        if problem:
            print(f"skip {clip.name}: {problem}")
            continue
        report["clips"][clip.stem] = marks
        print(f"{clip.name}: peak {marks['peakSpeed']:.0f}@{marks['peakTime']:.3f}  iframes "
              f"{marks['RDF_IFrameStart']:.3f}-{marks['RDF_IFrameEnd']:.3f}  attack/block {marks['RDF_AttackOpen']:.3f}  chain {marks['RDF_ChainOpen']:.3f}"
              f"  exit {marks['RDF_ExitOpen']:.3f}{describe_motion(marks)}")
    REPORT.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"wrote {REPORT}")


if __name__ == "__main__":
    main(sys.argv[1:])

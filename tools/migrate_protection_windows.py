"""Stage legacy RDF protection Start events as numeric metadata on frame zero.

Never edits the input directory. End events, other annotations and animation data are preserved.
Each staged HKX is unpacked again and compared to the intended XML before it is reported ready.
Usage: python tools/migrate_protection_windows.py --dir <clips> --out <staging>
"""
import argparse
import json
import math
from pathlib import Path

import annotate_clips as A

STARTS = ("RDF_IFrameStart", "RDF_PerfectStart")


def migrate_notes(notes):
    result = []
    seen = set()
    for time, text in notes:
        name = text.split(".", 1)[0]
        if name in STARTS:
            if name in seen:
                raise ValueError(f"multiple {name} events need an explicit timing choice")
            seen.add(name)
            if text == name:
                if not math.isfinite(time) or not 0 <= time <= 10:
                    raise ValueError(f"invalid {name} time: {time}")
                text = f"{name}.{time:.6f}"
                time = 0.0
            else:
                value = float(text[len(name) + 1:])
                if time != 0 or not math.isfinite(value) or not 0 <= value <= 10:
                    raise ValueError(f"{text} must be finite clip seconds on frame zero")
        result.append((time, text))
    return sorted(result, key=lambda note: note[0])


def migrate_xml(text):
    track = A.TRACK0.search(text)
    if not track:
        raise ValueError("no first annotation track")
    notes = [(float(t), s) for t, s in A.NOTE.findall(track.group(3))]
    migrated = migrate_notes(notes)
    body = (f'<hkparam name="annotations" numelements="{len(migrated)}">'
            f'{A.render(migrated)}\n\t\t\t\t\t</hkparam>')
    return text[:track.start()] + track.group(1) + body + track.group(4) + text[track.end():], migrated


def stage(source, destination):
    source, destination = source.resolve(), destination.resolve()
    if destination == source or source in destination.parents or destination in source.parents:
        raise ValueError("staging must be separate from the source directory")
    destination.mkdir(parents=True, exist_ok=True)
    report = []
    for clip in sorted(source.glob("*.hkx")):
        unpacked = destination / (clip.stem + ".before.xml")
        edited = destination / (clip.stem + ".xml")
        packed = destination / clip.name
        checked = destination / (clip.stem + ".check.xml")
        A.hkxc(clip, unpacked, "xml")
        original = unpacked.read_text(encoding="utf-8")
        expected, notes = migrate_xml(original)
        edited.write_text(expected, encoding="utf-8")
        A.hkxc(edited, packed, "amd64")
        A.hkxc(packed, checked, "xml")
        actual = checked.read_text(encoding="utf-8")
        # Full XML equality also checks transforms, root motion, End times and unrelated tracks.
        if actual != expected:
            raise ValueError(f"{clip.name}: round-trip XML differs from intended result")
        report.append({"source": str(clip), "staged": str(packed), "contract": "start-duration-v2",
                       "starts": {s: t for t, s in notes if s.split('.', 1)[0] in STARTS}})
    if not report:
        raise ValueError(f"no clips in {source}")
    (destination / "manifest.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    print(f"staged and verified {len(stage(args.dir, args.out))} clips -> {args.out}")

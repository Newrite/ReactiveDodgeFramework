"""Per-frame motion analysis of an uncompressed Skyrim animation clip (hkxc/serde-hkx XML).

For every frame prints the root-motion speed (from `animmotion` annotations), and the model-space
height of the pelvis and the head (forward kinematics against the skeleton), plus the clip's
existing non-motion annotations. Used to place RDF_* window annotations where the roll actually
ends and the character is upright again.

    python tools/clip_analyze.py clip.xml skeleton.xml [--bones "NPC Pelvis [Pelv]" "NPC Head [Head]"]
"""
import argparse
import math
import re
import sys

NUM = r"(-?[\d.]+(?:e-?\d+)?)"
TRANSFORM = re.compile(r"\(" + r"\s+".join([NUM] * 3) + r"\)\(" + r"\s+".join([NUM] * 4) + r"\)\(" + r"\s+".join([NUM] * 3) + r"\)")


def section(text, cls):
    # A top-level object runs to the next top-level <hkobject> (its own nested hkobjects, such as
    # annotation tracks or bones, close with </hkobject> first).
    start = text.index(f'class="{cls}"')
    end = text.find("\n\t\t<hkobject ", start)
    return text[start:end if end >= 0 else len(text)]


def param(block, name):
    m = re.search(rf'<hkparam name="{name}"[^>]*>(.*?)</hkparam>', block, re.S)
    return m.group(1) if m else None


def load_skeleton(path):
    block = section(open(path, encoding="utf-8").read(), "hkaSkeleton")
    parents = [int(x) for x in param(block, "parentIndices").split()]
    names = re.findall(r'<hkparam name="name">([^<]*)</hkparam>', block)
    return names[1:1 + len(parents)], parents  # names[0] is the skeleton's own name


def load_clip(path):
    text = open(path, encoding="utf-8").read()
    anim = section(text, "hkaInterleavedUncompressedAnimation")
    tracks = int(param(anim, "numberOfTransformTracks"))
    duration = float(param(anim, "duration"))
    poses = [tuple(float(v) for v in m.groups()) for m in TRANSFORM.finditer(param(anim, "transforms"))]
    frames = len(poses) // tracks
    notes = [(float(t), s) for t, s in re.findall(r'<hkparam name="time">([^<]*)</hkparam>\s*<hkparam name="text">([^<]*)</hkparam>', anim)]
    return duration, tracks, frames, poses, notes


def quat_rotate(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # v + 2w(q x v) + 2 q x (q x v)
    cx, cy, cz = y * vz - z * vy, z * vx - x * vz, x * vy - y * vx
    cx2, cy2, cz2 = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
    return (vx + 2 * (w * cx + cx2), vy + 2 * (w * cy + cy2), vz + 2 * (w * cz + cz2))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def model_position(bone, frame_poses, parents):
    chain = []
    while bone >= 0:
        chain.append(bone)
        bone = parents[bone]
    pos, rot = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)
    for b in reversed(chain):
        t = frame_poses[b][0:3]
        q = frame_poses[b][3:7]
        r = quat_rotate(rot, t)
        pos = (pos[0] + r[0], pos[1] + r[1], pos[2] + r[2])
        rot = quat_mul(rot, q)
    return pos


def motion_keys(notes):
    keys = [(0.0, (0.0, 0.0, 0.0))]
    for t, s in notes:
        if s.startswith("animmotion "):
            keys.append((t, tuple(float(v) for v in s.split()[1:4])))
    return keys


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("clip")
    ap.add_argument("skeleton")
    ap.add_argument("--bones", nargs="+", default=["NPC Pelvis [Pelv]", "NPC Head [Head]"])
    args = ap.parse_args(argv)

    bones, parents = load_skeleton(args.skeleton)
    duration, tracks, frames, poses, notes = load_clip(args.clip)
    indices = [bones.index(b) for b in args.bones]
    if max(indices) >= tracks:
        sys.exit("bone beyond the clip's tracks; is the binding not identity?")

    keys = motion_keys(notes)
    dt = duration / (frames - 1)
    print(f"duration {duration:.3f}s  frames {frames}  tracks {tracks}")
    print("   t     speed  dist   " + "  ".join(f"{b.split('[')[0].strip()[-10:]:>10}" for b in args.bones))
    for f in range(frames):
        t = f * dt
        frame_poses = poses[f * tracks:(f + 1) * tracks]
        heights = [model_position(i, frame_poses, parents)[2] for i in indices]
        prev = max((k for k in keys if k[0] <= t - dt + 1e-4), key=lambda k: k[0], default=keys[0])
        cur = max((k for k in keys if k[0] <= t + 1e-4), key=lambda k: k[0], default=keys[0])
        dist = math.dist(cur[1][:2], (0.0, 0.0))
        step = math.dist(cur[1][:2], prev[1][:2])
        speed = step / dt if f else 0.0
        print(f"{t:5.3f} {speed:7.1f} {dist:6.1f}   " + "  ".join(f"{h:10.1f}" for h in heights))
    others = [(t, s) for t, s in notes if not s.startswith("animmotion")]
    print("annotations:", others or "none besides animmotion")


if __name__ == "__main__":
    main(sys.argv[1:])

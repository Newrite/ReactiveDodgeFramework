#!/usr/bin/env python3
r"""Offline check of the Nemesis patch (behavior/Nemesis_Engine/mod/rdfm) without running Nemesis.

    python tools/check_rdf_nemesis.py

For every behavior file the patch touches:
  1. each edited node file, MOD_CODE blocks removed, is the vanilla node in Nemesis' layout (what Nemesis matches on)
  2. the patch is applied the way Nemesis does to the vanilla graph (hkxcmd dump): the blocks go into their params,
     the new nodes are added, $eventID[..]$ / $variableID[..]$ are resolved, numelements recounted
  3. the result is packed to a 64-bit hkx with serde-hkx and read back (it must be a valid Havok file)
  4. hkxcheck finds no new errors or warnings vs the vanilla graph, every RDF node is reachable by id, and the
     counts grew as the Pandora patch makes them grow (events, variables, root states, root wildcards, animations)
Outputs land in _work/nemesis_check/.
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_rdf_nemesis as N  # noqa: E402
import gen_rdf_patch as G  # noqa: E402
import hkxcheck as H  # noqa: E402

ROOT = N.ROOT
WORK = os.path.join(ROOT, "_work", "nemesis_check")
HKXC = r"S:\Programming\serde-hkx\target\release\hkxc.exe"
MOD = N.OUT
OK = [0, 0]


def check(msg, cond, detail=""):
    OK[0 if cond else 1] += 1
    print(("PASS " if cond else "FAIL ") + msg + (("  - " + detail) if detail and not cond else ""))


def blocks_of(text):
    """[(param name, [lines])] of the MOD_CODE blocks, and the text without them."""
    out, keep, param, cur = [], [], None, None
    for line in text.splitlines():
        m = re.match(r'^\t*<hkparam name="(\w+)" numelements=', line)  # the array the block belongs to
        if m and cur is None:
            param = m.group(1)
        if line.startswith("<!-- MOD_CODE ~%s~ OPEN -->" % N.CODE):
            cur = []
            continue
        if line == "<!-- CLOSE -->" and cur is not None:
            out.append((param, cur))
            cur = None
            continue
        if cur is not None:
            cur.append(line)
        else:
            keep.append(line)
    return out, "\n".join(keep) + "\n"


def node_span(text, node_id):
    i = text.find('<hkobject name="%s"' % node_id)
    start = text.rfind("\n", 0, i) + 1
    end = text.find("\n\t\t</hkobject>", i) + len("\n\t\t</hkobject>")
    return start, end


def insert_into(node, param, lines):
    src = node.split("\n")
    for n, line in enumerate(src):
        m = re.match(r'^(\t*)<hkparam name="%s"' % param, line)
        if m:
            close = m.group(1) + "</hkparam>"
            k = next(k for k in range(n + 1, len(src)) if src[k] == close)
            return "\n".join(src[:k] + lines + src[k:])
    raise KeyError(param)


def recount(text):
    """numelements of every array = its element count (Nemesis recounts the same way)."""
    out = text.split("\n")
    for n, line in enumerate(out):
        m = re.match(r'^(\t*)<hkparam name="\w+" numelements="(\d+)">$', line)
        if not m:
            continue
        depth = len(m.group(1)) + 1
        count, k = 0, n + 1
        while out[k] != m.group(1) + "</hkparam>":
            item = out[k]
            tabs = len(item) - len(item.lstrip("\t"))
            if tabs == depth:
                body = item.strip()
                if body.startswith("<hkobject") or body.startswith("<hkcstring"):
                    count += 1
                elif body.startswith("(") or body.startswith("#") or re.match(r"^-?\d", body):
                    count += len(re.findall(r"\([^)]*\)", body)) or len(body.split())
            k += 1
        out[n] = re.sub(r'numelements="\d+"', 'numelements="%d"' % count, line)
    return "\n".join(out)


def names_of(text, node_id, param):
    s, e = node_span(text, node_id)
    node = text[s:e]
    i = node.find('<hkparam name="%s"' % param)
    j = node.find("</hkparam>", i)
    return re.findall(r"<hkcstring>([^<]*)</hkcstring>", node[i:j])


def apply(folder, string_node):
    vanilla = open(N.VANILLA[folder], encoding="utf-8").read()
    text = vanilla
    d = os.path.join(MOD, folder)
    files = sorted(os.listdir(d))
    edited = [f[:-4] for f in files if re.match(r"^#\d{4}\.txt$", f)]
    new = [f for f in files if f.startswith("#%s$" % N.CODE)]
    for node_id in edited:
        ours = open(os.path.join(d, node_id + ".txt"), encoding="utf-8").read()
        blocks, rest = blocks_of(ours)
        check("%s %s: our file without the MOD_CODE blocks = the vanilla node in Nemesis' layout" % (folder, node_id),
              rest.strip("\n") == N.vanilla_node(folder, node_id).strip("\n"))
        s, e = node_span(text, node_id)
        node = text[s:e]
        for param, lines in blocks:
            node = insert_into(node, param, lines)
        text = text[:s] + node + text[e:]
    body = "\n".join(open(os.path.join(d, f), encoding="utf-8").read().rstrip("\n") for f in new)
    k = text.rfind("\t</hksection>")
    text = text[:k] + body + "\n" + text[k:]
    text = recount(text)
    if string_node:
        events = names_of(text, string_node, "eventNames")
        variables = names_of(text, string_node, "variableNames")
        missing = []

        def sub(kind, table):
            def f(m):
                if m.group(1) not in table:
                    missing.append(m.group(1))
                    return "-1"
                return str(table.index(m.group(1)))
            return f
        text = re.sub(r"\$eventID\[([^\]]+)\]\$", sub("event", events), text)
        text = re.sub(r"\$variableID\[([^\]]+)\]\$", sub("variable", variables), text)
        check("%s: every $eventID / $variableID resolves" % folder, not missing, str(sorted(set(missing))[:5]))
    # Nemesis ids are pointers like any other: give the new ones hkxcmd-style numbers past the last vanilla one
    last = max(int(x) for x in re.findall(r'<hkobject name="#(\d{4})"', vanilla))
    ids = {}
    for m in re.finditer(r'name="(#%s\$\d+)"' % re.escape(N.CODE), text):
        ids.setdefault(m.group(1), "#%04d" % (last + 1 + len(ids)))
    text = re.sub(r"#%s\$\d+" % re.escape(N.CODE), lambda m: ids.get(m.group(0), m.group(0)), text)
    check("%s: %d new nodes, %d edited" % (folder, len(new), len(edited)), len(ids) == len(new))
    return vanilla, text


def roundtrip(stem, text):
    os.makedirs(WORK, exist_ok=True)
    xml, hkx, back = (os.path.join(WORK, stem + ext) for ext in (".xml", ".hkx", ".back.xml"))
    open(xml, "w", encoding="utf-8").write(text)
    r1 = subprocess.run([HKXC, "convert", "-i", xml, "-o", hkx, "-v", "amd64"], capture_output=True, text=True)
    r2 = subprocess.run([HKXC, "convert", "-i", hkx, "-o", back, "-v", "xml"], capture_output=True, text=True)
    ok = r1.returncode == 0 and r2.returncode == 0 and os.path.isfile(back)
    check("%s: packs to a 64-bit hkx and reads back (serde-hkx)" % stem, ok, (r1.stderr or r1.stdout)[-300:] + (r2.stderr or "")[-200:])
    return back if ok else None


def main():
    if not os.path.isdir(MOD):
        sys.exit("run tools/gen_rdf_nemesis.py first")
    # --- 3P 0_master
    van, txt = apply("0_master", G.V["STRING"])
    back, vback = roundtrip("0_master", txt), roundtrip("0_master_vanilla", van)
    if back and vback:
        g, gb = H.Graph(back), H.Graph(vback)
        check("0_master: events +%d" % len(G.EVENTS), len(g.event_names) == len(gb.event_names) + len(G.EVENTS)
              and all(e in g.ev_index for e in G.EVENTS))
        check("0_master: variables +%d" % len(G.VARIABLES), len(g.variable_names) == len(gb.variable_names) + len(G.VARIABLES)
              and g.n_var_infos == len(g.variable_names) and g.n_word == len(g.variable_names))
        root = [x for x in H.state_machines(g) if x.text("name") == "RootBehaviorGraph"][0]
        rootb = [x for x in H.state_machines(gb) if x.text("name") == "RootBehaviorGraph"][0]
        names = [g.objs[s].text("name") for s in H.sm_states(g, root)]
        check("0_master: root states +2 (RDF_Dodge_A/B)", len(names) == len(H.sm_states(gb, rootb)) + 2
              and G.STATE_A[0] in names and G.STATE_B[0] in names)
        rep, repb = H.Report(), H.Report()
        H.check(g, rep)
        H.check(gb, repb)
        check("0_master: hkxcheck no new errors / warnings vs vanilla (%d/%d)" % (len(rep.errors), len(rep.warns)),
              len(rep.errors) == len(repb.errors) and len(rep.warns) == len(repb.warns), "; ".join(rep.errors[:3]))
    # --- 1P 0_master
    folder = os.path.join("_1stperson", "0_master")
    van, txt = apply(folder, G.V_FP["STRING"])
    back, vback = roundtrip("fp_0_master", txt), roundtrip("fp_0_master_vanilla", van)
    if back and vback:
        g, gb = H.Graph(back), H.Graph(vback)
        check("1P 0_master: events +%d, variables +%d" % (len(G.EVENTS), len(G.VARIABLES)),
              len(g.event_names) == len(gb.event_names) + len(G.EVENTS) and len(g.variable_names) == len(gb.variable_names) + len(G.VARIABLES))
        root = [x for x in H.state_machines(g) if x.text("name") == "RootBehaviorGraph"][0]
        rootb = [x for x in H.state_machines(gb) if x.text("name") == "RootBehaviorGraph"][0]
        check("1P 0_master: root states +2", len(H.sm_states(g, root)) == len(H.sm_states(gb, rootb)) + 2)
        rep, repb = H.Report(), H.Report()
        H.check(g, rep)
        H.check(gb, repb)
        check("1P 0_master: hkxcheck no new errors / warnings vs vanilla (%d/%d)" % (len(rep.errors), len(rep.warns)),
              len(rep.errors) == len(repb.errors) and len(rep.warns) == len(repb.warns), "; ".join(rep.errors[:3]))
    # --- 1hm_behavior (dodge-cancel events for attack clips), 3P and 1P
    for folder, stem in (("1hm_behavior", "1hm_behavior"), (os.path.join("_1stperson", "1hm_behavior"), "fp_1hm_behavior")):
        van, txt = apply(folder, G.V_1HM["STRING"])
        back, vback = roundtrip(stem, txt), roundtrip(stem + "_vanilla", van)
        if back and vback:
            g, gb = H.Graph(back), H.Graph(vback)
            events = G.EVENTS + G.ONEHAND_START_EVENTS + G.CANCEL_EVENTS
            check("%s: events +%d, eventInfos match" % (folder, len(events)),
                  len(g.event_names) == len(gb.event_names) + len(events) and all(len(g.ev_index.get(e, [])) == 1 for e in events)
                  and g.n_event_infos == len(g.event_names))
            # the 1hm dodge states, 3P and 1P (BEHAVIOR_INTEGRATION.md 7)
            sm = [s for s in H.state_machines(g) if s.text("name") == "1HM_Behavior"][0]
            states = {g.objs[s].intval("stateId"): g.objs[s] for s in H.sm_states(g, sm)}
            ok = all(sid in states and states[sid].text("name") == nm
                     and states[sid].text("transitions") == states[G.ONEHAND_READY_ID].text("transitions")
                     for nm, sid in G.ONEHAND_STATES)
            check("%s: RDF_1H_Dodge_A/B leave by 1HM_Ready_State's transition array" % folder, ok)
            wl = {g.event_name(int(d["eventId"])) for d in H.trans_array(g, sm.text("wildcardTransitions"))}
            check("%s: 1HM_Behavior wildcards for %s and RDF_DodgeExit" % (folder, "/".join(G.ONEHAND_START_EVENTS)),
                  set(G.ONEHAND_START_EVENTS) | {"RDF_DodgeExit"} <= wl)
            rep, repb = H.Report(), H.Report()
            H.check(g, rep)
            H.check(gb, repb)
            check("%s: hkxcheck no new errors / warnings vs vanilla" % folder,
                  len(rep.errors) == len(repb.errors) and len(rep.warns) == len(repb.warns), "; ".join(rep.errors[:3]))
    # --- characters
    for ch in ("defaultmale", "defaultfemale"):
        van, txt = apply(ch, None)
        added = [a for a in names_of(txt, "#0029", "animationNames") if a not in names_of(van, "#0029", "animationNames")]
        check("%s: animationNames +%d (the RDF clips)" % (ch, len(G.CLIPS)),
              added == ["%s\\%s.hkx" % (G.ANIM_DIR, c) for c in G.CLIPS], str(added[:3]))
        roundtrip(ch, txt)
    print("\n%d/%d checks passed" % (OK[0], OK[0] + OK[1]))
    return 1 if OK[1] else 0


if __name__ == "__main__":
    sys.exit(main())

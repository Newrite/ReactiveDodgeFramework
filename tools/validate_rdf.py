#!/usr/bin/env python3
"""Validate the RDF patch by comparing two REAL Pandora runs staged by tools/stage_pandora.py.

  python tools/stage_pandora.py --label baseline                                   # current modlist, no rdf
  python tools/stage_pandora.py --label rdf --rdf behavior/Pandora_Engine/mod/rdf   # + rdf
  python tools/validate_rdf.py                                                      # exit 0 = all checks pass

Checks (each printed as PASS/FAIL):
  A  Engine.log of the rdf run has no problem line for the rdf mod, and none beyond the baseline's
  B  every hkx except 0_Master/DefaultMale/DefaultFemale is byte-identical to the baseline run
  C  required events/variables exist; no macro resolved to -1 (RDF nodes use real indices)
  D  states 9701/9702: names, enter/exit events, generator chain, 16 clips (paths, speed binding, end trigger)
  E  transitions of both states and the root wildcard exactly as designed (events, targets, effects, flags)
  F  all 16 clip names are in DefaultMale AND DefaultFemale animationNames; hkxcheck over every graph is clean
  G  nothing lost: per state machine, #states and #transitions equal baseline except RootBehaviorGraph (+2 / +1);
     objects present in the baseline but missing in the rdf run are only the 5 expected modified nodes
"""
import collections
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import hkxcheck as H  # noqa: E402
import gen_rdf_patch as G  # noqa: E402

HKXC = r"S:\Programming\serde-hkx\target\release\hkxc.exe"
OUT = os.path.join(ROOT, "_work", "pandora_test_out")
CHAR = os.path.join("meshes", "actors", "character")
FILES = [("Behaviors", "0_Master.hkx"), ("Behaviors", "mt_behavior.hkx"), ("Behaviors", "1hm_behavior.hkx"),
         ("Behaviors", "1hm_locomotion.hkx"), ("Behaviors", "blockbehavior.hkx"), ("Behaviors", "bashbehavior.hkx"),
         ("Behaviors", "sprintbehavior.hkx"), ("Behaviors", "staggerbehavior.hkx"), ("Behaviors", "idlebehavior.hkx"),
         ("Behaviors", "weapequip.hkx"), ("Behaviors", "magicbehavior.hkx"), ("Behaviors", "shout_behavior.hkx"),
         ("Characters", "DefaultMale.hkx"), ("Characters Female", "DefaultFemale.hkx")]

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + (("  - " + detail) if detail else ""))


def to_xml(label):
    d = os.path.join(OUT, label + "_xml")
    os.makedirs(d, exist_ok=True)
    for sub, f in FILES:
        src = os.path.join(OUT, label, CHAR, sub, f)
        dst = os.path.join(d, os.path.splitext(f)[0] + ".xml")
        r = subprocess.run([HKXC, "convert", "-i", src, "-o", dst, "-v", "xml"], capture_output=True, text=True)
        if r.returncode or not os.path.exists(dst):
            print("hkxc failed for", src, r.stdout[-200:], r.stderr[-200:])
            sys.exit(2)
    return d


def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()


EVENT_ID_PARAMS = {"eventId", "id"}
VARIABLE_ID_PARAMS = {"variableIndex"}


def param_text(g, elem):
    """Text of a param with event / variable ids replaced by their names: another mod appending its
    events after ours renumbers them, which must not count as a change to its objects."""
    parts = []
    for e in elem.iter():
        name = e.get("name")
        text = (e.text or "").strip()
        if name in EVENT_ID_PARAMS and re.fullmatch(r"-?\d+", text):
            parts.append("E:" + str(g.event_name(int(text))))
        elif name in VARIABLE_ID_PARAMS and re.fullmatch(r"-?\d+", text):
            i = int(text)
            parts.append("V:" + (g.variable_names[i] if 0 <= i < len(g.variable_names) else text))
        elif text:
            parts.append(text)
    t = re.sub(r"#\d+", "#", " ".join(parts))
    return re.sub(r"\s+", " ", t).strip()


def canon(g):
    c = collections.Counter()
    ex = {}
    for o in g.order:
        key = (o.cls, tuple((k, param_text(g, p)) for k, p in o.params.items()))
        c[key] += 1
        ex.setdefault(key, o)
    return c, ex


def sm_stats(g):
    out = {}
    for sm in H.state_machines(g):
        states = H.sm_states(g, sm)
        n = len(H.trans_array(g, sm.text("wildcardTransitions", "null")))
        for sp in states:
            so = g.objs.get(sp)
            if so:
                n += len(H.trans_array(g, so.text("transitions", "null")))
        key = (sm.text("name"), sm.cls)
        out.setdefault(key, []).append((len(states), n))
    return out


def main():
    base_x, rdf_x = to_xml("baseline"), to_xml("rdf")
    # ---- A
    bad = [ln for ln in open(os.path.join(OUT, "rdf", "Engine.log"), encoding="utf-8", errors="replace")
           if ("FAILED" in ln or "SKIPPED" in ln or "Already exists" in ln) and "FNIS Parser" not in ln]
    badb = [ln for ln in open(os.path.join(OUT, "baseline", "Engine.log"), encoding="utf-8", errors="replace")
            if ("FAILED" in ln or "SKIPPED" in ln or "Already exists" in ln) and "FNIS Parser" not in ln]
    ran = any("Reactive Dodge Framework" in ln for ln in open(os.path.join(OUT, "rdf", "Engine.log"), errors="replace"))
    check("A1 rdf mod ran (listed in Engine.log)", ran)
    check("A2 no new failure lines vs baseline", sorted(bad) == sorted(badb), "%d vs %d problem lines" % (len(bad), len(badb)))
    # ---- B
    diffs = []
    for sub, f in FILES:
        a, b = os.path.join(OUT, "baseline", CHAR, sub, f), os.path.join(OUT, "rdf", CHAR, sub, f)
        if md5(a) != md5(b):
            diffs.append(f)
    check("B  hkx changed only in 0_Master/DefaultMale/DefaultFemale", sorted(diffs) == ["0_Master.hkx", "DefaultFemale.hkx", "DefaultMale.hkx"], str(diffs))

    g = H.Graph(os.path.join(rdf_x, "0_Master.xml"))
    gb = H.Graph(os.path.join(base_x, "0_Master.xml"))
    male = H.Graph(os.path.join(rdf_x, "DefaultMale.xml"))
    female = H.Graph(os.path.join(rdf_x, "DefaultFemale.xml"))
    # ---- C
    for e in G.EVENTS:
        # Position is not checked: mods after ours in Pandora's priority append their events later.
        check("C  event %s present once" % e, e in g.ev_index and len(g.ev_index[e]) == 1 and e not in gb.ev_index, "index %s" % (g.ev_index.get(e),))
    for v, typ, init in G.VARIABLES:
        check("C  variable %s present" % v, v in g.var_index, "index %s" % g.var_index.get(v))
    check("C  eventNames/eventInfos grew by %d" % len(G.EVENTS), len(g.event_names) == len(gb.event_names) + len(G.EVENTS) and g.n_event_infos == len(g.event_names))
    check("C  variableNames/variableInfos/wordVariableValues grew by %d" % len(G.VARIABLES),
          len(g.variable_names) == len(gb.variable_names) + len(G.VARIABLES) and g.n_var_infos == len(g.variable_names) and g.n_word == len(g.variable_names))
    check("C  DF_IsDodging not created (absent in modlist)", "DF_IsDodging" not in g.var_index)
    # initial values
    vv = g.by_class["hkbVariableValueSet"][0].params["wordVariableValues"].findall("hkobject")
    init_ok = True
    for v, typ, init in G.VARIABLES:
        i = g.var_index[v][0]
        got = int(vv[i].find("hkparam[@name='value']").text)
        want = G.f2i(init) if typ == "VARIABLE_TYPE_REAL" else int(init)
        init_ok &= got == want
    check("C  initial values (0, 1.0f, 0)", init_ok)
    types = {x.get("name"): None for x in []}
    infos = g.by_class["hkbBehaviorGraphData"][0].params["variableInfos"].findall("hkobject")
    tl = [infos[g.var_index[v][0]].find("hkparam[@name='type']").text for v, _, _ in G.VARIABLES]
    check("C  variable types", tl == [t for _, t, _ in G.VARIABLES], str(tl))

    # ---- D
    root = [s for s in H.state_machines(g) if s.text("name") == "RootBehaviorGraph"][0]
    states = {}
    for sp in H.sm_states(g, root):
        so = g.objs[sp]
        states[so.intval("stateId")] = so
    for nm, sid in (G.STATE_A, G.STATE_B):
        so = states.get(sid)
        check("D  state %d exists" % sid, so is not None)
        if so is None:
            continue
        check("D  %d name/enable" % sid, so.text("name") == nm and so.text("enable") == "true", so.text("name"))
        ent = H.notify_events(g, so.text("enterNotifyEvents"))
        ext = H.notify_events(g, so.text("exitNotifyEvents"))
        check("D  %d enter=RDF_DodgeEntered exit=RDF_DodgeLeft" % sid,
              [e.split("(")[0] for e in ent] == ["RDF_DodgeEntered"] and [e.split("(")[0] for e in ext] == ["RDF_DodgeLeft"], "%s / %s" % (ent, ext))
        mg = g.objs[so.text("generator")]
        mod = g.objs[mg.text("modifier")]
        sel = g.objs[mg.text("generator")]
        check("D  %d generator = ModifierGenerator{BSIsActiveModifier, ManualSelector}" % sid,
              mg.cls == "hkbModifierGenerator" and mod.cls == "BSIsActiveModifier" and sel.cls == "hkbManualSelectorGenerator")
        bs = g.objs[mod.text("variableBindingSet")]
        binds = [(b.find("hkparam[@name='memberPath']").text, g.var_name(int(b.find("hkparam[@name='variableIndex']").text)))
                 for b in bs.params["bindings"].findall("hkobject")]
        check("D  %d IsActive bindings" % sid, binds == [("bIsActive0", "bAnimationDriven"), ("bIsActive1", "RDF_IsDodging")], str(binds))
        sbs = g.objs[sel.text("variableBindingSet")]
        sb = [(b.find("hkparam[@name='memberPath']").text, g.var_name(int(b.find("hkparam[@name='variableIndex']").text)))
              for b in sbs.params["bindings"].findall("hkobject")]
        check("D  %d selector binds selectedGeneratorIndex<-RDF_DodgeClip" % sid, sb == [("selectedGeneratorIndex", "RDF_DodgeClip")], str(sb))
        clips = sel.text("generators").split()
        ok = len(clips) == 16
        problems = []
        for i, cp in enumerate(clips):
            c = g.objs[cp]
            exp = "%s\\%s.hkx" % (G.ANIM_DIR, G.CLIPS[i])
            if c.cls != "hkbClipGenerator" or c.text("animationName") != exp or c.text("mode") != "MODE_SINGLE_PLAY":
                problems.append("clip %d %s" % (i, c.text("animationName")))
            cb = g.objs[c.text("variableBindingSet")]
            b = cb.params["bindings"].findall("hkobject")
            if len(b) != 1 or b[0].find("hkparam[@name='memberPath']").text != "playbackSpeed" or g.var_name(int(b[0].find("hkparam[@name='variableIndex']").text)) != "RDF_DodgeSpeed":
                problems.append("clip %d speed binding" % i)
            ta = g.objs[c.text("triggers")].params["triggers"].findall("hkobject")
            if len(ta) != 1:
                problems.append("clip %d triggers" % i)
            else:
                t = {x.get("name"): x for x in ta[0].findall("hkparam")}
                eid = int(t["event"].find("hkobject/hkparam[@name='id']").text)
                if g.event_name(eid) != "RDF_DodgeClipEnd" or t["relativeToEndOfClip"].text != "true" or abs(float(t["localTime"].text) - G.CLIP_END_TRIGGER_TIME) > 1e-6:
                    problems.append("clip %d trigger %s" % (i, g.event_name(eid)))
        check("D  %d: 16 clips in order, SINGLE_PLAY, speed<-RDF_DodgeSpeed, RDF_DodgeClipEnd @ end%+.2f" % (sid, G.CLIP_END_TRIGGER_TIME), ok and not problems, "; ".join(problems[:4]))

    # ---- E
    def fxdesc(ptr):
        o = g.objs[ptr]
        return (o.text("name"), round(float(o.text("duration")), 6), o.text("flags"), o.text("selfTransitionMode"), o.text("blendCurve"), o.text("endMode"))
    exp_in = (G.FX_IN[0], G.FX_IN[1], G.FX_IN[2], G.FX_IN[3], "BLEND_CURVE_SMOOTH", "END_MODE_NONE")
    exp_chain = (G.FX_CHAIN[0], G.FX_CHAIN[1], G.FX_CHAIN[2], G.FX_CHAIN[3], "BLEND_CURVE_SMOOTH", "END_MODE_NONE")
    exp_out = (G.FX_OUT[0], G.FX_OUT[1], G.FX_OUT[2], G.FX_OUT[3], "BLEND_CURVE_SMOOTH", "END_MODE_NONE")
    wl = H.trans_array(g, root.text("wildcardTransitions"))
    for event, (_, state_id) in G.START_EVENTS:
        mine = [d for d in wl if g.event_name(int(d["eventId"])) == event]
        check("E  exactly one root wildcard %s" % event, len(mine) == 1)
        if mine:
            d = mine[0]
            check("E  %s -> %d, flags LOCAL_WILDCARD, effect RDF_DodgeInBlend" % (event, state_id),
                  d["toStateId"] == str(state_id) and "FLAG_IS_LOCAL_WILDCARD" in d["flags"] and fxdesc(d["transition"]) == exp_in, "%s %s" % (d["flags"], fxdesc(d["transition"])))
    n_base_wild = len(H.trans_array(gb, [s for s in H.state_machines(gb) if s.text("name") == "RootBehaviorGraph"][0].text("wildcardTransitions")))
    check("E  root wildcards: baseline %d + %d" % (n_base_wild, len(G.START_EVENTS)), len(wl) == n_base_wild + len(G.START_EVENTS))
    n_exit = len(G.EXIT_EVENTS) * (len(G.V["RETURN_CONDS"]) if G.RETURN_MODE == "vanilla" else 1)
    for nm, sid, other in ((G.STATE_A[0], 9701, 9702), (G.STATE_B[0], 9702, 9701)):
        ts = H.trans_array(g, states[sid].text("transitions"))
        ev = [g.event_name(int(d["eventId"])) for d in ts]
        check("E  %d transition count = 1 chain + %d exit + 1 stagger" % (sid, n_exit), len(ts) == 1 + n_exit + 1, str(len(ts)))
        ch = [d for d in ts if g.event_name(int(d["eventId"])) == "RDF_DodgeChain"]
        check("E  %d RDF_DodgeChain -> %d with RDF_DodgeChainBlend" % (sid, other), len(ch) == 1 and ch[0]["toStateId"] == str(other) and fxdesc(ch[0]["transition"]) == exp_chain)
        for e in G.EXIT_EVENTS:
            xs = [d for d in ts if g.event_name(int(d["eventId"])) == e]
            ok = len(xs) == (4 if G.RETURN_MODE == "vanilla" else 1) and all(d["toStateId"] == "14" and fxdesc(d["transition"]) == exp_out for d in xs)
            check("E  %d %s -> Default_State(14) x%d with RDF_DodgeOutBlend" % (sid, e, len(xs)), ok)
        if G.RETURN_MODE == "vanilla":
            xs = [d for d in ts if g.event_name(int(d["eventId"])) == "RDF_DodgeExit"]
            conds = [g.objs[d["condition"]].text("expression") for d in xs]
            check("E  %d vanilla return conditions resolve to the 4 Stagger_State expressions" % sid, conds == [c[3] for c in G.V["RETURN_CONDS"]], str(conds))
        sp = [d for d in ts if g.event_name(int(d["eventId"])) == "StaggerPlayer"]
        check("E  %d StaggerPlayer -> PlayerStagger_State(48)" % sid, len(sp) == 1 and sp[0]["toStateId"] == "48")
        check("E  %d no eventId -1 / unresolved effect" % sid, all(int(d["eventId"]) >= 0 and d["transition"] in g.objs for d in ts))

    # stagger / ragdoll / death / jump wildcards still reachable from our states
    wl_names = {g.event_name(int(d["eventId"])) for d in wl}
    for e in ("staggerStart", "WardBreak", "Ragdoll", "JumpStandingStart", "SwimStart", "bleedOutStart", "JumpFall"):
        check("E  vanilla root wildcard %s still present (applies from RDF states)" % e, e in wl_names)
    master = [s for s in H.state_machines(g) if s.text("name") == "Master_Behavior"][0]
    mw = {g.event_name(int(d["eventId"])) for d in H.trans_array(g, master.text("wildcardTransitions"))}
    check("E  Master_Behavior wildcards (DeathAnim, Ragdoll, RagdollInstant) intact", {"DeathAnim", "Ragdoll", "RagdollInstant"} <= mw)

    # ---- F
    for cname, ch in (("DefaultMale", male), ("DefaultFemale", female)):
        have = {x.lower() for x in ch.animation_names}
        miss = [c for c in G.CLIPS if ("%s\\%s.hkx" % (G.ANIM_DIR, c)).lower() not in have]
        check("F  %s has all 16 RDF animationNames" % cname, not miss, str(miss))
    base_m = H.Graph(os.path.join(base_x, "DefaultMale.xml"))
    check("F  DefaultMale grew by exactly 16", len(male.animation_names) == len(base_m.animation_names) + 16, "%d -> %d" % (len(base_m.animation_names), len(male.animation_names)))
    for sub, f in FILES:
        stem = os.path.splitext(f)[0]
        if stem.startswith("Default"):
            continue
        gg = H.Graph(os.path.join(rdf_x, stem + ".xml"))
        bgg = H.Graph(os.path.join(base_x, stem + ".xml"))
        r1, r2 = H.Report(), H.Report()
        H.check(gg, r1)
        H.check(bgg, r2)
        check("F  hkxcheck %s: no new errors vs baseline" % stem, len(r1.errors) == len(r2.errors) and len(r1.warns) == len(r2.warns), "%d/%d errors" % (len(r1.errors), len(r2.errors)))
    r = H.Report()
    H.check(g, r)
    check("F  hkxcheck 0_Master (rdf): 0 errors, 0 warnings", not r.errors and not r.warns)
    # character-bound clip check on 0_Master
    miss = [o.text("animationName") for o in g.by_class.get("hkbClipGenerator", []) if o.text("animationName").lower() not in {x.lower() for x in male.animation_names}]
    check("F  every 0_Master clip animationName is in DefaultMale", not miss, str(miss[:3]))

    # ---- G
    sb, sg = sm_stats(gb), sm_stats(g)
    bad = []
    for k, v in sb.items():
        if k == ("RootBehaviorGraph", "hkbStateMachine"):
            continue
        if sg.get(k) != v:
            bad.append((k, v, sg.get(k)))
    check("G  every other state machine has identical #states/#transitions (%d machines)" % (len(sb) - 1), not bad, str(bad[:2]))
    kb, kg = sb[("RootBehaviorGraph", "hkbStateMachine")][0], sg[("RootBehaviorGraph", "hkbStateMachine")][0]
    exp_new_tr = len(G.START_EVENTS) + 2 * (1 + n_exit + 1)
    check("G  RootBehaviorGraph: states %d -> %d (+2), transitions %d -> %d (+%d)" % (kb[0], kg[0], kb[1], kg[1], exp_new_tr),
          kg[0] == kb[0] + 2 and kg[1] == kb[1] + exp_new_tr)
    cb, eb = canon(gb)
    cg, eg = canon(g)
    lost = cb - cg
    added = cg - cb
    lost_cls = sorted(k[0] for k in lost.elements())
    check("G  objects lost vs baseline = only the 5 modified nodes", lost_cls == sorted(["hkbBehaviorGraphData", "hkbBehaviorGraphStringData", "hkbVariableValueSet", "hkbStateMachine", "hkbStateMachineTransitionInfoArray"]), str(lost_cls))
    n_new = G.gen_master(set())[1]
    check("G  objects added = %d new + 5 modified" % n_new, sum(added.values()) == n_new + 5, str(sum(added.values())))
    sm_changed = [k for k in lost if k[0] == "hkbStateMachine"]
    check("G  the lost hkbStateMachine is RootBehaviorGraph", len(sm_changed) == 1 and dict(sm_changed[0][1]).get("name") == "RootBehaviorGraph")
    print()
    print("%d/%d checks passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

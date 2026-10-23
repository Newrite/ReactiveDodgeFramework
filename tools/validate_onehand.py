#!/usr/bin/env python3
"""Validate the --onehand prototype (the dodge inside 1hm_behavior) against the regular RDF run, both REAL staged Pandora runs.

  python tools/gen_rdf_patch.py _work/proto_1hm/rdf --onehand
  python tools/stage_pandora.py --label rdf --rdf behavior/Pandora_Engine/mod/rdf      # regular patch (reference)
  python tools/stage_pandora.py --label proto1h --rdf _work/proto_1hm/rdf              # prototype
  python tools/validate_onehand.py [--ref rdf] [--label proto1h]                       # exit 0 = all checks pass

Checks:
  A  Engine.log: the mod ran, no failure line beyond the reference run's
  B  only 1hm_behavior.hkx and magicbehavior.hkx (3P, 1P; the magic layer: validate_rdf.py) differ from the reference run
  C  1hm_behavior: our events / variables present once each, tables consistent
  D  1HM_Behavior: +2 states (9701/9702), both leave by 1HM_Ready_State's OWN transition array (same object, unchanged content),
     enter / exit notify events, generator chain (BSIsActiveModifier bindings, 2 clip choosers of 18 clips, stance masks bound to
     1hm's RightArm / LeftArm character properties); every clip is in DefaultMale
  E  1HM_Behavior wildcards: +3 (start A, start B, exit to Ready under RDF_IsDodging == 1), the vanilla ones intact
  F  hkxcheck 1hm_behavior: no new errors / warnings
  G  nothing else changed (beyond the bow layer in Bow_Attack_LowerBodyBehavior): other state machines keep #states / #transitions, objects lost = only the modified nodes
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hkxcheck as H  # noqa: E402
import gen_rdf_patch as G  # noqa: E402
import validate_rdf as VR  # noqa: E402

check = VR.check


def problem_lines(label):
    """Engine.log problem lines, event indices blanked (our events renumber the ones appended after them)."""
    return sorted(re.sub(r"Index > \d+", "Index > #", ln) for ln in
                  open(os.path.join(VR.OUT, label, "Engine.log"), encoding="utf-8", errors="replace")
                  if ("FAILED" in ln or "SKIPPED" in ln or "Already exists" in ln) and "FNIS Parser" not in ln)


def same_graph(ref_x, new_x, stem):
    """Byte-identical, or the same objects once ids are named (Pandora's output order is not stable on every modlist)."""
    a, b = H.Graph(os.path.join(ref_x, stem + ".xml")), H.Graph(os.path.join(new_x, stem + ".xml"))

    def objects(g):   # a character file's animationNames come in a run-dependent order: compared as a set below
        return VR.canon(g)[0] - VR.canon_class(g, "hkbCharacterStringData")
    return objects(a) == objects(b) and sorted(a.animation_names) == sorted(b.animation_names)


def trans_dicts(g, arr):
    return H.trans_array(g, arr) if arr and arr != "null" else []


def describe(g, d):
    fx = g.objs.get(d["transition"])
    cond = g.objs.get(d["condition"])
    return (g.event_name(int(d["eventId"])), d["toStateId"], d["toNestedStateId"], d["flags"],
            fx.text("name") if fx is not None else d["transition"],
            cond.text("expression") if cond is not None else d["condition"])


def find_state(g, sm, sid):
    for sp in H.sm_states(g, sm):
        if g.objs[sp].intval("stateId") == sid:
            return g.objs[sp]
    return None


def check_state(g, male, st, nm, fp=False):
    tag = "D  %s%s" % ("1P " if fp else "", nm)
    enter = [g.event_name(int(d.find("hkparam[@name='id']").text)) for d in
             g.objs[st.text("enterNotifyEvents")].params["events"].findall("hkobject")]
    exit_ = [g.event_name(int(d.find("hkparam[@name='id']").text)) for d in
             g.objs[st.text("exitNotifyEvents")].params["events"].findall("hkobject")]
    check(tag + " enter notify RDF_DodgeEntered + InterruptCast, exit notify RDF_DodgeLeft",
          enter == ["RDF_DodgeEntered", "InterruptCast"] and exit_ == ["RDF_DodgeLeft"], "%s / %s" % (enter, exit_))
    mg = g.objs[st.text("generator")]
    isact = g.objs[mg.text("modifier")]
    check(tag + " ModifierGenerator{BSIsActiveModifier bAnimationDriven, RDF_IsDodging}",
          mg.cls == "hkbModifierGenerator" and isact.cls == "BSIsActiveModifier"
          and VR.bind_list(g, isact) == [("bIsActive0", "bAnimationDriven", "BINDING_TYPE_VARIABLE"),
                                         ("bIsActive1", "RDF_IsDodging", "BINDING_TYPE_VARIABLE")], str(VR.bind_list(g, isact)))
    outer = g.objs[mg.text("generator")]
    c0, c1 = VR.children_of(g, outer)
    main_sel = g.objs[c0.text("generator")]
    path = G.fp_clip_path if fp else None
    probs = VR.dodge_selector_problems(g, main_sel, path)
    check(tag + " main clip chooser: state machine of %d clips bound to RDF_DodgeClip / RDF_DodgeSpeed" % len(G.CLIPS), not probs, str(probs[:3]))
    arm = g.objs[c1.text("generator")]
    check(tag + " arm layer: parametric blend on RDF_ArmBlend",
          VR.bind_list(g, arm) == [("blendParameter", "RDF_ArmBlend", "BINDING_TYPE_VARIABLE")], str(VR.bind_list(g, arm)))
    p0, p1 = VR.children_of(g, arm)
    probs = VR.dodge_selector_problems(g, g.objs[p0.text("generator")], path)
    check(tag + " arm-layer clip chooser: %d clips" % len(G.CLIPS), not probs, str(probs[:3]))
    stance = g.objs[p1.text("generator")]
    if fp:   # 1P: the weapon stance machine itself (a weapon is always out inside 1hm), chosen by iRightHandType
        check(tag + " stance: state machine on iRightHandType (no currentDefaultState chooser in 1hm)",
              stance.cls == "hkbStateMachine" and VR.bind_list(g, stance) == [("startStateId", "iRightHandType", "BINDING_TYPE_VARIABLE")],
              "%s %s" % (stance.cls, VR.bind_list(g, stance)))
    else:
        props = [VR.bind_list(g, ch) for ch in VR.children_of(g, stance)]
        check(tag + " stance masks bound to 1hm character properties RightArm / LeftArm",
              props == [[("boneWeights", ("prop", "RightArm"), "BINDING_TYPE_CHARACTER_PROPERTY")],
                        [("boneWeights", ("prop", "LeftArm"), "BINDING_TYPE_CHARACTER_PROPERTY")]], str(props))
    names = {x.lower() for x in male.animation_names}
    clips = [g.objs[x] for x in VR.reachable(g, st.text("generator")) if g.objs[x].cls == "hkbClipGenerator"]
    miss = sorted({c.text("animationName") for c in clips if c.text("animationName").lower() not in names})
    check(tag + " all %d clips of the state are in the character's animationNames" % len(clips), not miss, str(miss[:3]))


def check_file(ref_x, new_x, stem, char, fp):
    g = H.Graph(os.path.join(new_x, stem + ".xml"))
    gr = H.Graph(os.path.join(ref_x, stem + ".xml"))
    male = H.Graph(os.path.join(new_x, char + ".xml"))
    tag = "[%s] " % stem
    # ---- C
    bow = G.BOW_EVENTS   # the bow layer, BEHAVIOR_INTEGRATION.md 8
    new_events = [e for e in G.EVENTS + G.ONEHAND_START_EVENTS + bow if e not in gr.ev_index]
    for e in G.EVENTS + G.ONEHAND_START_EVENTS + G.CANCEL_EVENTS + bow:
        check(tag + "C  event %s present once" % e, len(g.ev_index.get(e, [])) == 1)
    check(tag + "C  eventNames +%d, eventInfos match" % len(new_events),
          len(g.event_names) == len(gr.event_names) + len(new_events) and g.n_event_infos == len(g.event_names))
    for v, _t, _i in G.VARIABLES:
        check(tag + "C  variable %s present once" % v, len(g.var_index.get(v, [])) == 1)
    check(tag + "C  variable tables consistent (+%d)" % len(G.VARIABLES),
          len(g.variable_names) == len(gr.variable_names) + len(G.VARIABLES) and g.n_var_infos == len(g.variable_names))
    # ---- D
    sm = [s for s in H.state_machines(g) if s.text("name") == "1HM_Behavior"][0]
    smr = [s for s in H.state_machines(gr) if s.text("name") == "1HM_Behavior"][0]
    check(tag + "D  1HM_Behavior states %d -> %d (+2)" % (len(H.sm_states(gr, smr)), len(H.sm_states(g, sm))),
          len(H.sm_states(g, sm)) == len(H.sm_states(gr, smr)) + 2)
    ready, ready_r = find_state(g, sm, G.ONEHAND_READY_ID), find_state(gr, smr, G.ONEHAND_READY_ID)
    check(tag + "D  stateId 0 is 1HM_Ready_State", ready is not None and ready.text("name") == "1HM_Ready_State")
    rt = [describe(g, d) for d in trans_dicts(g, ready.text("transitions"))]
    rtr = [describe(gr, d) for d in trans_dicts(gr, ready_r.text("transitions"))]
    check(tag + "D  Ready's transitions unchanged vs the reference (%d, every mod's additions included)" % len(rt), rt == rtr,
          "%d vs %d" % (len(rt), len(rtr)))
    for nm, sid in G.ONEHAND_STATES:
        st = find_state(g, sm, sid)
        check(tag + "D  %s is stateId %d" % (nm, sid), st is not None and st.text("name") == nm)
        if st is None:
            continue
        check(tag + "D  %s leaves by Ready's own transition array (%s)" % (nm, ready.text("transitions")),
              st.text("transitions") == ready.text("transitions"), st.text("transitions"))
        check_state(g, male, st, nm, fp)
    # ---- E
    wl = [describe(g, d) for d in trans_dicts(g, sm.text("wildcardTransitions"))]
    wlr = [describe(gr, d) for d in trans_dicts(gr, smr.text("wildcardTransitions"))]
    check(tag + "E  1HM_Behavior wildcards %d -> %d (+3)" % (len(wlr), len(wl)), len(wl) == len(wlr) + 3)
    check(tag + "E  vanilla wildcards intact", all(w in wl for w in wlr))
    for event, (nm, sid) in zip(G.ONEHAND_START_EVENTS, G.ONEHAND_STATES):
        m = [w for w in wl if w[0] == event]
        check(tag + "E  %s -> %d, local wildcard, RDF_DodgeInBlend" % (event, sid),
              len(m) == 1 and m[0][1] == str(sid) and "FLAG_IS_LOCAL_WILDCARD" in m[0][3] and m[0][4] == G.FX_IN[0], str(m))
    m = [w for w in wl if w[0] == "RDF_DodgeExit"]
    check(tag + "E  RDF_DodgeExit -> Ready(0) under '%s', RDF_DodgeOutBlend" % G.ONEHAND_EXIT_EXPR,
          len(m) == 1 and m[0][1] == "0" and m[0][5] == G.ONEHAND_EXIT_EXPR and m[0][4] == G.FX_OUT[0]
          and "FLAG_DISABLE_CONDITION" not in m[0][3], str(m))
    # ---- F
    r, rr = H.Report(), H.Report()
    H.check(g, r)
    H.check(gr, rr)
    check(tag + "F  hkxcheck 1hm_behavior: no new errors / warnings", len(r.errors) == len(rr.errors) and len(r.warns) == len(rr.warns),
          "%d/%d errors, %d/%d warnings" % (len(r.errors), len(rr.errors), len(r.warns), len(rr.warns)))
    # ---- G
    sb, sg = VR.sm_stats(gr), VR.sm_stats(g)
    mine = {("1HM_Behavior", "hkbStateMachine"), ("Bow_Attack_LowerBodyBehavior", "hkbStateMachine")}
    bad = [(k, v, sg.get(k)) for k, v in sb.items() if k not in mine and sg.get(k) != v]
    check(tag + "G  every other state machine keeps #states / #transitions (%d)" % (len(sb) - len(mine)), not bad, str(bad[:2]))
    cb, _ = VR.canon(gr)
    cg, _ = VR.canon(g)
    lost = sorted(k[0] for k in (cb - cg).elements())
    want_lost = ["hkbBehaviorGraphData", "hkbBehaviorGraphStringData", "hkbVariableValueSet", "hkbStateMachine",
                 "hkbStateMachineTransitionInfoArray"]
    # the bow layer: Bow_Attack_LowerBodyBehavior and its standing / moving states' transition arrays
    want_lost += ["hkbStateMachine"] + ["hkbStateMachineTransitionInfoArray"] * len((G.V_BOW_FP if fp else G.V_BOW)["STATE_TRANSITIONS"])
    want_lost.sort()
    check(tag + "G  objects lost vs the reference = only the %d modified nodes" % len(want_lost), lost == want_lost, str(lost))
    n_new = G.gen_onehand(fp)[1]
    added = sum((cg - cb).values())
    check(tag + "G  objects added = %d new + %d modified" % (n_new, len(want_lost)), added == n_new + len(want_lost), str(added))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default="rdf")
    ap.add_argument("--label", default="proto1h")
    a = ap.parse_args()
    ref_x, new_x = VR.to_xml(a.ref), VR.to_xml(a.label)
    # ---- A
    ran = any("Reactive Dodge Framework" in ln for ln in open(os.path.join(VR.OUT, a.label, "Engine.log"), errors="replace"))
    check("A  rdf mod ran", ran)
    pl, pr = problem_lines(a.label), problem_lines(a.ref)
    check("A  no failure lines beyond the reference run", pl == pr, "%d vs %d" % (len(pl), len(pr)))
    # ---- B
    diffs = [VR.xstem(sub, f) for sub, f in VR.FILES
             if VR.md5(os.path.join(VR.OUT, a.ref, VR.CHAR, sub, f)) != VR.md5(os.path.join(VR.OUT, a.label, VR.CHAR, sub, f))
             and not same_graph(ref_x, new_x, VR.xstem(sub, f))]
    check("B  only 1hm_behavior and magicbehavior (3P and 1P) changed vs the reference",
          sorted(diffs) == ["1hm_behavior", "fp_1hm_behavior", "fp_magicbehavior", "magicbehavior"], str(diffs))
    for stem, char, fp in (("1hm_behavior", "DefaultMale", False), ("fp_1hm_behavior", "fp_FirstPerson", True)):
        check_file(ref_x, new_x, stem, char, fp)
    print()
    print("%d/%d checks passed" % (sum(VR.results), len(VR.results)))
    return 0 if all(VR.results) else 1


if __name__ == "__main__":
    sys.exit(main())

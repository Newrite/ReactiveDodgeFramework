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
         ("Characters", "DefaultMale.hkx"), ("Characters Female", "DefaultFemale.hkx"),
         ("_1stperson\\Behaviors", "0_Master.hkx"), ("_1stperson\\Behaviors", "mt_behavior.hkx"),
         ("_1stperson\\Behaviors", "1hm_behavior.hkx"), ("_1stperson\\Behaviors", "magicbehavior.hkx"),
         ("_1stperson\\Characters", "FirstPerson.hkx")]


def xstem(sub, f):
    """xml file stem of a dumped file (first-person files are prefixed fp_)."""
    stem = os.path.splitext(f)[0]
    return "fp_" + stem if sub.startswith("_1stperson") else stem

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + (("  - " + detail) if detail else ""))


def to_xml(label):
    d = os.path.join(OUT, label + "_xml")
    os.makedirs(d, exist_ok=True)
    for sub, f in FILES:
        src = os.path.join(OUT, label, CHAR, sub, f)
        dst = os.path.join(d, xstem(sub, f) + ".xml")
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



def bind_list(g, o):
    """[(memberPath, variableName-or-('prop', name), bindingType)] of an object's binding set."""
    bs = g.objs.get(o.text("variableBindingSet", "null"))
    out = []
    if bs is None:
        return out
    for b in bs.params["bindings"].findall("hkobject"):
        d = {x.get("name"): (x.text or "").strip() for x in b.findall("hkparam")}
        idx = int(d["variableIndex"])
        if "CHARACTER_PROPERTY" in d["bindingType"]:
            nm = g.char_prop_names[idx] if 0 <= idx < len(g.char_prop_names) else "?%d" % idx
            out.append((d["memberPath"], ("prop", nm), d["bindingType"]))
        else:
            out.append((d["memberPath"], g.var_name(idx), d["bindingType"]))
    return out


def floats(o, name):
    return [float(x) for x in (o.params[name].text or "").split()]


def char_masks(ch):
    """name -> list of bone weights from the character-property hkbBoneWeightArray values of a character file."""
    names = [c.text for c in ch.by_class["hkbCharacterStringData"][0].params["characterPropertyNames"].findall("hkcstring")]
    vs = ch.by_class["hkbVariableValueSet"][0]
    words = vs.params["wordVariableValues"].findall("hkobject")
    variants = vs.params["variantVariableValues"].text.split()
    infos = ch.by_class["hkbCharacterData"][0].params["characterPropertyInfos"].findall("hkobject")
    out = {}
    for i, n in enumerate(names):
        t = infos[i].find("hkparam[@name='type']").text
        if "POINTER" in t or "VARIANT" in t:
            o = ch.objs.get(variants[int(words[i].find("hkparam[@name='value']").text)])
            if o is not None and o.cls == "hkbBoneWeightArray":
                out[n] = floats(o, "boneWeights")
    return out


def dodge_selector_problems(g, sm, path=None):
    """The dodge clip chooser must be a state machine (not a ManualSelector): startStateId <- RDF_DodgeClip, 16 states."""
    problems = []
    if sm.cls != "hkbStateMachine":
        return ["not a state machine (%s)" % sm.cls]
    if bind_list(g, sm) != [("startStateId", "RDF_DodgeClip", "BINDING_TYPE_VARIABLE")]:
        problems.append("binding %s" % bind_list(g, sm))
    want = {"startStateMode": "START_STATE_MODE_DEFAULT", "selfTransitionMode": "SELF_TRANSITION_MODE_NO_TRANSITION",
            "wildcardTransitions": "null", "startStateChooser": "null", "syncVariableIndex": "-1", "startStateId": "0",
            "maxSimultaneousTransitions": "32"}
    for k, v in want.items():
        if sm.text(k) != v:
            problems.append("%s=%s" % (k, sm.text(k)))
    states = sm.text("states").split()
    if len(states) != len(G.CLIPS):
        problems.append("%d states" % len(states))
    for i, sp in enumerate(states):
        so = g.objs[sp]
        if so.cls != "hkbStateMachineStateInfo" or so.intval("stateId") != i:
            problems.append("state %d stateId %s" % (i, so.text("stateId")))
        for k in ("enterNotifyEvents", "exitNotifyEvents", "transitions", "variableBindingSet"):
            if so.text(k) != "null":
                problems.append("state %d %s not null" % (i, k))
        c = g.objs[so.text("generator")]
        exp = path(G.CLIPS[i]) if path else "%s\\%s.hkx" % (G.ANIM_DIR, G.CLIPS[i])
        if c.cls != "hkbClipGenerator" or c.text("animationName") != exp or c.text("mode") != "MODE_SINGLE_PLAY":
            problems.append("clip %d %s" % (i, c.text("animationName")))
        if bind_list(g, c) != [("playbackSpeed", "RDF_DodgeSpeed", "BINDING_TYPE_VARIABLE")]:
            problems.append("clip %d speed binding" % i)
        if c.text("triggers") not in ("null", None):
            problems.append("clip %d has triggers (no clip-end exit by design)" % i)
    return problems


def children_of(g, blender):
    return [g.objs[c] for c in blender.text("children").split()]


def stance_entry(g, ptr):
    """clip file, or ('sel', var, [entries]) for a nested selector."""
    o = g.objs[ptr]
    if o.cls == "hkbClipGenerator":
        assert o.text("mode") == "MODE_LOOPING", "stance clip %s not looping" % o.text("name")
        return o.text("animationName")
    assert o.cls == "hkbManualSelectorGenerator"
    return ("sel", bind_list(g, o)[0][1], [stance_entry(g, c) for c in o.text("generators").split()])


def reachable(g, ptr):
    seen, todo = set(), [ptr]
    while todo:
        x = todo.pop()
        o = g.objs.get(x)
        if o is None or x in seen:
            continue
        seen.add(x)
        for _path, _anc, prm in g.walk_params(o):
            for t in (prm.text or "").split():
                if t.startswith("#") and t in g.objs:
                    todo.append(t)
    return seen


def check_arm_layer(g, male, female, masks_real, sid, outer):
    """H: arm stance layer of one RDF state. `outer` = the hkbBlenderGenerator under the ModifierGenerator."""
    tag = "H  %d" % sid
    check(tag + " outer blender: plain blend (flags 0), 2 children", outer.cls == "hkbBlenderGenerator" and outer.text("flags") == "0" and len(children_of(g, outer)) == 2)
    c0, c1 = children_of(g, outer)
    right, left = masks_real["RightArm"], masks_real["LeftArm"]
    n = len(right)
    union = [1.0 if (right[i] > 0 or left[i] > 0) else 0.0 for i in range(n)]
    check(tag + " masks: RightArm and LeftArm of the real character files do not overlap, length %d" % n,
          not any(right[i] > 0 and left[i] > 0 for i in range(n)) and n == G.N_BONES)
    m0, m1 = floats(g.objs[c0.text("boneWeights")], "boneWeights"), floats(g.objs[c1.text("boneWeights")], "boneWeights")
    check(tag + " child0 (dodge) mask = complement of RightArm|LeftArm; child1 mask = RightArm|LeftArm",
          m1 == union and m0 == [1.0 - u for u in union], "len %d/%d vs %d" % (len(m0), len(m1), n))
    check(tag + " per bone: dodge + right + left masks sum to 1 (result independent of weight normalisation)",
          all(abs(m0[i] + right[i] + left[i] - 1.0) < 1e-6 for i in range(n)))
    check(tag + " child0 weight 1 worldFromModel 1; child1 weight 1 worldFromModel 0 (root motion only from the dodge clip)",
          (float(c0.text("weight")), float(c0.text("worldFromModelWeight")), float(c1.text("weight")), float(c1.text("worldFromModelWeight"))) == (1.0, 1.0, 1.0, 0.0))
    sel_main = g.objs[c0.text("generator")]
    pr = dodge_selector_problems(g, sel_main)
    check(tag + " child0 generator = dodge state machine (startStateId <- RDF_DodgeClip, 16 states in order, speed binding, no triggers)", not pr, "; ".join(pr[:3]))
    arm = g.objs[c1.text("generator")]
    check(tag + " ArmLayer = parametric blender (flags 16), blendParameter <- RDF_ArmBlend",
          arm.cls == "hkbBlenderGenerator" and arm.text("flags") == "16" and bind_list(g, arm) == [("blendParameter", "RDF_ArmBlend", "BINDING_TYPE_VARIABLE")], str(bind_list(g, arm)))
    p0, p1 = children_of(g, arm)
    check(tag + " ArmLayer children: weight 0 = dodge copy, weight 1 = stance; no masks; worldFromModel 0",
          float(p0.text("weight")) == 0.0 and float(p1.text("weight")) == 1.0 and p0.text("boneWeights") == "null" and p1.text("boneWeights") == "null"
          and p0.text("worldFromModelWeight") == p1.text("worldFromModelWeight") == "0.000000")
    sel_arm = g.objs[p0.text("generator")]
    pr = dodge_selector_problems(g, sel_arm)
    check(tag + " ArmLayer child0 = its OWN dodge state machine (not shared with the outer one)", not pr and sel_arm.name != sel_main.name and not (reachable(g, sel_arm.name) & reachable(g, sel_main.name)), "; ".join(pr[:3]))
    stance = g.objs[p1.text("generator")]
    check(tag + " StanceBlender: plain blend, 2 children", stance.cls == "hkbBlenderGenerator" and stance.text("flags") == "0" and len(children_of(g, stance)) == 2)
    sr, sl = children_of(g, stance)
    for side, ch_ in (("R", sr), ("L", sl)):
        want_prop, _ = G.CHAR_PROP[side]
        check(tag + " stance %s: boneWeights <- character property %s (vanilla binding pattern), weight 1, worldFromModel 0" % (side, want_prop),
              bind_list(g, ch_) == [("boneWeights", ("prop", want_prop), "BINDING_TYPE_CHARACTER_PROPERTY")] and ch_.text("boneWeights") == "null"
              and ch_.text("weight") == "1.000000" and ch_.text("worldFromModelWeight") == "0.000000", str(bind_list(g, ch_)))
        sel = g.objs[ch_.text("generator")]
        check(tag + " stance %s selector bound to %s, 13 entries" % (side, G.HAND_VARS[side]),
              sel.cls == "hkbManualSelectorGenerator" and bind_list(g, sel) == [("selectedGeneratorIndex", G.HAND_VARS[side], "BINDING_TYPE_VARIABLE")]
              and len(sel.text("generators").split()) == 13, str(bind_list(g, sel)))
        table = G.STANCE_RIGHT if side == "R" else G.STANCE_LEFT
        got = [stance_entry(g, e) for e in sel.text("generators").split()]
        exp = []
        for key in table:
            if key == "1H":
                exp.append(("sel", G.HAND_VARS["L"], [G.STANCE_CLIP_FILES[k] for k in G.STANCE_NESTED_LEFT]))
            else:
                exp.append(G.STANCE_CLIP_FILES[key])
        check(tag + " stance %s entries match the stance table" % side, got == exp, "")
        files = {x for x in got if isinstance(x, str)} | {f for x in got if isinstance(x, tuple) for f in x[2]}
        for cname, ch_file in (("DefaultMale", male), ("DefaultFemale", female)):
            have = {a.lower() for a in ch_file.animation_names}
            miss = sorted(f for f in files if f.lower() not in have)
            check(tag + " stance %s: all %d stance clips exist in %s animationNames" % (side, len(files), cname), not miss, str(miss))
    # every binding of the whole generator tree resolves (no -1)
    bad = []
    for ptr in reachable(g, outer.name):
        o = g.objs[ptr]
        if o.cls == "hkbVariableBindingSet":
            for b in o.params["bindings"].findall("hkobject"):
                if int(b.find("hkparam[@name='variableIndex']").text) < 0:
                    bad.append(ptr)
    check(tag + " no binding with index -1 anywhere below the state (%d nodes)" % len(reachable(g, outer.name)), not bad, str(bad[:3]))


def check_fp(rdf_x, base_x):
    """P: first-person graph (firstperson~0_master)."""
    g = H.Graph(os.path.join(rdf_x, "fp_0_Master.xml"))
    gb = H.Graph(os.path.join(base_x, "fp_0_Master.xml"))
    fpc = H.Graph(os.path.join(rdf_x, "fp_FirstPerson.xml"))
    fpb = H.Graph(os.path.join(base_x, "fp_FirstPerson.xml"))
    tag = "P  "
    for e in G.EVENTS:
        check(tag + "1P event %s present once" % e, e in g.ev_index and len(g.ev_index[e]) == 1 and e not in gb.ev_index)
    for v, typ, init in G.VARIABLES:
        check(tag + "1P variable %s present once" % v, v in g.var_index and len(g.var_index[v]) == 1 and v not in gb.var_index)
    check(tag + "1P eventNames/eventInfos grew by %d" % len(G.EVENTS), len(g.event_names) == len(gb.event_names) + len(G.EVENTS) and g.n_event_infos == len(g.event_names))
    check(tag + "1P variableNames/variableInfos/wordVariableValues grew by %d" % len(G.VARIABLES),
          len(g.variable_names) == len(gb.variable_names) + len(G.VARIABLES) and g.n_var_infos == len(g.variable_names) and g.n_word == len(g.variable_names))
    vv = g.by_class["hkbVariableValueSet"][0].params["wordVariableValues"].findall("hkobject")
    infos = g.by_class["hkbBehaviorGraphData"][0].params["variableInfos"].findall("hkobject")
    ok = True
    for v, typ, init in G.VARIABLES:
        i = g.var_index[v][0]
        want = G.f2i(init) if typ == "VARIABLE_TYPE_REAL" else int(init)
        ok &= int(vv[i].find("hkparam[@name='value']").text) == want and infos[i].find("hkparam[@name='type']").text == typ
    check(tag + "1P variable initial values and types", ok)
    for v in ("bAnimationDriven", "iRightHandType", "iLeftHandType", "currentDefaultState", "bAttached"):
        check(tag + "1P graph has vanilla variable %s (not created by RDF)" % v, v in g.var_index and v in gb.var_index)
    root = [x for x in H.state_machines(g) if x.text("name") == "RootBehaviorGraph"][0]
    rootb = [x for x in H.state_machines(gb) if x.text("name") == "RootBehaviorGraph"][0]
    states = {g.objs[x].intval("stateId"): g.objs[x] for x in H.sm_states(g, root)}
    check(tag + "1P root RootBehaviorGraph: states %d -> %d (+2)" % (len(H.sm_states(gb, rootb)), len(states)), len(states) == len(H.sm_states(gb, rootb)) + 2)
    have = {a.lower() for a in fpc.animation_names}
    for nm, sid in (G.STATE_A, G.STATE_B):
        so = states.get(sid)
        check(tag + "1P state %d exists" % sid, so is not None)
        if so is None:
            continue
        ent = [e.split("(")[0] for e in H.notify_events(g, so.text("enterNotifyEvents"))]
        ext = [e.split("(")[0] for e in H.notify_events(g, so.text("exitNotifyEvents"))]
        check(tag + "1P %d name/enable/enter/exit" % sid, so.text("name") == nm and so.text("enable") == "true" and ent == ["RDF_DodgeEntered", "InterruptCast"] and ext == ["RDF_DodgeLeft"])
        mg = g.objs[so.text("generator")]
        mod = g.objs[mg.text("modifier")]
        blend = g.objs[mg.text("generator")]
        check(tag + "1P %d generator = ModifierGenerator{BSIsActiveModifier(bAnimationDriven, RDF_IsDodging), parametric blender <- RDF_ArmBlend}" % sid,
              mg.cls == "hkbModifierGenerator" and mod.cls == "BSIsActiveModifier" and blend.cls == "hkbBlenderGenerator"
              and blend.text("flags") == "16" and bind_list(g, blend) == [("blendParameter", "RDF_ArmBlend", "BINDING_TYPE_VARIABLE")]
              and [(m, v) for m, v, _t in bind_list(g, mod)] == [("bIsActive0", "bAnimationDriven"), ("bIsActive1", "RDF_IsDodging")])
        b0, b1 = children_of(g, blend)
        check(tag + "1P %d blend children: weight 0 = dodge clip, weight 1 = stance; no masks; worldFromModel 0" % sid,
              float(b0.text("weight")) == 0.0 and float(b1.text("weight")) == 1.0 and b0.text("boneWeights") == "null"
              and b1.text("boneWeights") == "null" and b0.text("worldFromModelWeight") == b1.text("worldFromModelWeight") == "0.000000")
        fp_sm = g.objs[b0.text("generator")]
        pr = dodge_selector_problems(g, fp_sm, G.fp_clip_path)
        check(tag + "1P %d dodge chooser: startStateId <- RDF_DodgeClip, 16 states -> vanilla 1P dodge clips, speed binding" % sid, not pr, "; ".join(pr[:3]))
        fp_files = {G.fp_clip_path(c).lower() for c in G.CLIPS}
        check(tag + "1P %d dodge clips are in FirstPerson animationNames %s" % (sid, sorted(fp_files)), fp_files <= have, str(fp_files - have))
        drawn = g.objs[b1.text("generator")]
        check(tag + "1P %d stance child is a state-machine chooser" % sid, drawn.cls == "hkbStateMachine")
        # outer chooser: startStateId <- currentDefaultState, states = Default_Behavior ids; sheathed ids -> MT_Idle, drawn ids -> stance chooser
        ids = [g.objs[x].intval("stateId") for x in drawn.text("states").split()]
        kinds, stance_ptrs, bad_d = {}, set(), []
        for x in drawn.text("states").split():
            st = g.objs[x]
            gen = g.objs[st.text("generator")]
            if gen.cls == "hkbClipGenerator":
                kinds[st.intval("stateId")] = "MT"
                if gen.text("animationName") != G.STANCE_CLIP_FILES["MT"] or gen.text("mode") != "MODE_LOOPING":
                    bad_d.append(gen.text("animationName"))
            else:
                kinds[st.intval("stateId")] = "STANCE"
                stance_ptrs.add(gen.name)
        check(tag + "1P %d drawn chooser: startStateId <- currentDefaultState, stateIds %s, sheathed ids -> looping MT_Idle, drawn ids -> ONE stance chooser" % (sid, sorted(G.FP_DRAWN)),
              bind_list(g, drawn) == [("startStateId", G.FP_DRAWN_VAR, "BINDING_TYPE_VARIABLE")] and ids == sorted(G.FP_DRAWN)
              and kinds == G.FP_DRAWN and not bad_d and len(stance_ptrs) == 1 and drawn.text("wildcardTransitions") == "null"
              and drawn.text("startStateMode") == "START_STATE_MODE_DEFAULT", str((bind_list(g, drawn), ids, kinds, bad_d)))
        choose = g.objs[next(iter(stance_ptrs))] if stance_ptrs else drawn
        mt_file = G.STANCE_CLIP_FILES["MT"]
        check(tag + "1P %d MT_Idle is in FirstPerson animationNames" % sid, mt_file.lower() in have)

        def sm_entries(sm, var):
            assert sm.cls == "hkbStateMachine", sm.cls
            assert bind_list(g, sm) == [("startStateId", var, "BINDING_TYPE_VARIABLE")], bind_list(g, sm)
            assert sm.text("startStateMode") == "START_STATE_MODE_DEFAULT" and sm.text("selfTransitionMode") == "SELF_TRANSITION_MODE_NO_TRANSITION"
            assert sm.text("wildcardTransitions") == "null"
            out = []
            for i, sp in enumerate(sm.text("states").split()):
                st = g.objs[sp]
                assert st.intval("stateId") == i and st.text("transitions") == "null" and st.text("enterNotifyEvents") == "null"
                gen = g.objs[st.text("generator")]
                if gen.cls == "hkbClipGenerator":
                    assert gen.text("mode") == "MODE_LOOPING" and gen.text("triggers") == "null"
                    out.append(gen.text("animationName"))
                else:
                    out.append(("sm", [x for x in sm_entries(gen, "iLeftHandType")]))
            return out
        try:
            got = sm_entries(choose, "iRightHandType")
            exp = []
            for key in G.FP_STANCE_RIGHT:
                if key in G.FP_NESTED:
                    exp.append(("sm", [G.STANCE_CLIP_FILES[k] for k in G.FP_NESTED[key]]))
                else:
                    exp.append(G.STANCE_CLIP_FILES[key])
            check(tag + "1P %d stance chooser: startStateId<-iRightHandType, 13 states = hand types, nested iLeftHandType machines, looping clips, table matches" % sid, got == exp and len(got) == 13)
            files = {x for x in got if isinstance(x, str)} | {f for x in got if isinstance(x, tuple) for f in x[1]}
            miss = sorted(f for f in files if f.lower() not in have)
            check(tag + "1P %d all %d stance clips are in FirstPerson animationNames (%d names)" % (sid, len(files), len(fpc.animation_names)), not miss, str(miss))
        except AssertionError as e:
            check(tag + "1P %d stance chooser structure" % sid, False, str(e))
        bad = []
        for ptr in reachable(g, so.name):
            o = g.objs[ptr]
            if o.cls == "hkbVariableBindingSet":
                for b in o.params["bindings"].findall("hkobject"):
                    if int(b.find("hkparam[@name='variableIndex']").text) < 0:
                        bad.append(ptr)
        check(tag + "1P %d no binding with index -1 below the state (%d nodes)" % (sid, len(reachable(g, so.name))), not bad)

    def fxdesc(ptr):
        o = g.objs[ptr]
        return (o.text("name"), round(float(o.text("duration")), 6), o.text("flags"), o.text("selfTransitionMode"))
    exp_in, exp_chain, exp_out = G.FX_IN, G.FX_CHAIN, G.FX_OUT
    wl = H.trans_array(g, root.text("wildcardTransitions"))
    n_base_wild = len(H.trans_array(gb, rootb.text("wildcardTransitions")))
    for event, (_, state_id) in G.START_EVENTS:
        mine = [d for d in wl if g.event_name(int(d["eventId"])) == event]
        check(tag + "1P wildcard %s -> %d LOCAL_WILDCARD RDF_DodgeInBlend" % (event, state_id),
              len(mine) == 1 and mine[0]["toStateId"] == str(state_id) and "FLAG_IS_LOCAL_WILDCARD" in mine[0]["flags"] and fxdesc(mine[0]["transition"]) == exp_in)
    check(tag + "1P root wildcards %d + %d" % (n_base_wild, len(G.START_EVENTS)), len(wl) == n_base_wild + len(G.START_EVENTS))
    n_exit = len(G.EXIT_EVENTS) * (len(G.V_FP["RETURN_CONDS"]) if G.RETURN_MODE == "vanilla" else 1)
    for nm, sid, other in ((G.STATE_A[0], 9701, 9702), (G.STATE_B[0], 9702, 9701)):
        ts = H.trans_array(g, states[sid].text("transitions"))
        check(tag + "1P %d transition count 1 chain + %d exit + 1 stagger" % (sid, n_exit), len(ts) == 1 + n_exit + 1, str(len(ts)))
        ch = [d for d in ts if g.event_name(int(d["eventId"])) == "RDF_DodgeChain"]
        check(tag + "1P %d RDF_DodgeChain -> %d RDF_DodgeChainBlend" % (sid, other), len(ch) == 1 and ch[0]["toStateId"] == str(other) and fxdesc(ch[0]["transition"]) == exp_chain)
        for e in G.EXIT_EVENTS:
            xs = [d for d in ts if g.event_name(int(d["eventId"])) == e]
            check(tag + "1P %d %s -> Default_State(14) x%d RDF_DodgeOutBlend" % (sid, e, len(xs)), len(xs) == 4 and all(d["toStateId"] == "14" and fxdesc(d["transition"]) == exp_out for d in xs))
        xs = [d for d in ts if g.event_name(int(d["eventId"])) == "RDF_DodgeExit"]
        conds = [g.objs[d["condition"]].text("expression") for d in xs]
        check(tag + "1P %d return conditions = the 4 vanilla 1P Stagger_State expressions" % sid, conds == [c[3] for c in G.V_FP["RETURN_CONDS"]], str(conds))
        sp = [d for d in ts if g.event_name(int(d["eventId"])) == "StaggerPlayer"]
        check(tag + "1P %d StaggerPlayer -> PlayerStagger_State(48), vanilla effect" % sid, len(sp) == 1 and sp[0]["toStateId"] == "48" and g.objs[sp[0]["transition"]].text("name") == "FastBlendResetTransition")
        check(tag + "1P %d no eventId -1 / unresolved effect" % sid, all(int(d["eventId"]) >= 0 and d["transition"] in g.objs for d in ts))
    wl_names = {g.event_name(int(d["eventId"])) for d in wl}
    for e in ("staggerStart", "WardBreak", "Ragdoll", "JumpStandingStart", "JumpDirectionalStart", "SwimStart", "bleedOutStart", "JumpFall"):
        check(tag + "1P vanilla root wildcard %s still present" % e, e in wl_names)
    # nothing lost
    sb, sg = sm_stats(gb), sm_stats(g)
    bad = [(k, v, sg.get(k)) for k, v in sb.items() if k != ("RootBehaviorGraph", "hkbStateMachine") and sg.get(k) != v]
    check(tag + "1P every other state machine has identical #states/#transitions (%d machines)" % (len(sb) - 1), not bad, str(bad[:2]))
    kb, kg = sb[("RootBehaviorGraph", "hkbStateMachine")][0], sg[("RootBehaviorGraph", "hkbStateMachine")][0]
    check(tag + "1P RootBehaviorGraph transitions %d -> %d (+%d)" % (kb[1], kg[1], len(G.START_EVENTS) + 2 * (1 + n_exit + 1)), kg[0] == kb[0] + 2 and kg[1] == kb[1] + len(G.START_EVENTS) + 2 * (1 + n_exit + 1))
    cb, _ = canon(gb)
    cg, _ = canon(g)
    lost = sorted(k[0] for k in (cb - cg).elements())
    n_new = G.gen_master(set(), fp=True)[1]
    check(tag + "1P objects lost vs baseline = only the 5 modified nodes", lost == sorted(["hkbBehaviorGraphData", "hkbBehaviorGraphStringData", "hkbVariableValueSet", "hkbStateMachine", "hkbStateMachineTransitionInfoArray"]), str(lost))
    check(tag + "1P objects added = %d new + 5 modified" % n_new, sum((cg - cb).values()) == n_new + 5, str(sum((cg - cb).values())))
    r, rb = H.Report(), H.Report()
    H.check(g, r)
    H.check(gb, rb)
    check(tag + "1P hkxcheck 0_Master: no new errors/warnings vs baseline", len(r.errors) == len(rb.errors) and len(r.warns) == len(rb.warns), "%d/%d errors" % (len(r.errors), len(rb.errors)))
    check(tag + "1P FirstPerson character file unchanged (animationNames %d)" % len(fpc.animation_names), fpc.animation_names == fpb.animation_names)


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
            diffs.append(xstem(sub, f))
    check("B  hkx changed only in 0_Master/DefaultMale/DefaultFemale and first-person 0_Master",
          sorted(diffs) == ["0_Master", "DefaultFemale", "DefaultMale", "fp_0_Master"], str(diffs))

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

    masks_real = char_masks(male)
    stale = [o.name for o in g.by_class.get("hkbManualSelectorGenerator", []) if ("selectedGeneratorIndex", "RDF_DodgeClip", "BINDING_TYPE_VARIABLE") in bind_list(g, o)]
    check("H  no hkbManualSelectorGenerator bound to RDF_DodgeClip remains (stale-child pose step)", not stale, str(stale))
    check("H  DefaultFemale RightArm/LeftArm masks identical to DefaultMale", {k: v for k, v in char_masks(female).items() if k in ("RightArm", "LeftArm")} == {k: v for k, v in masks_real.items() if k in ("RightArm", "LeftArm")})
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
        check("D  %d enter=RDF_DodgeEntered+InterruptCast exit=RDF_DodgeLeft" % sid,
              [e.split("(")[0] for e in ent] == ["RDF_DodgeEntered", "InterruptCast"] and [e.split("(")[0] for e in ext] == ["RDF_DodgeLeft"], "%s / %s" % (ent, ext))
        mg = g.objs[so.text("generator")]
        mod = g.objs[mg.text("modifier")]
        outer = g.objs[mg.text("generator")]
        check("D  %d generator = ModifierGenerator{BSIsActiveModifier, Blender}" % sid,
              mg.cls == "hkbModifierGenerator" and mod.cls == "BSIsActiveModifier" and outer.cls == "hkbBlenderGenerator")
        binds = [(m, v) for m, v, _t in bind_list(g, mod)]
        check("D  %d IsActive bindings" % sid, binds == [("bIsActive0", "bAnimationDriven"), ("bIsActive1", "RDF_IsDodging")], str(binds))
        check_arm_layer(g, male, female, masks_real, sid, outer)

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
        stem = xstem(sub, f)
        if stem.startswith("Default") or stem.endswith("FirstPerson"):
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
    check_fp(rdf_x, base_x)
    print()
    print("%d/%d checks passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

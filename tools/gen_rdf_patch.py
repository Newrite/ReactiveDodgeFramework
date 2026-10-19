#!/usr/bin/env python3
"""Generate the RDF (Reactive Dodge Framework) Pandora native patch.

  python tools/gen_rdf_patch.py [OUT_DIR]        default OUT_DIR = behavior/Pandora_Engine/mod/rdf

Writes   info.xml
         patches/0_master.xml               events, variables, 2 root states, root wildcard
         patches/defaultmale_character.xml  16 animation names
         patches/defaultfemale_character.xml

All node ids are NEMESIS ids of the VANILLA graph (see docs/research/behavior-pipeline.md section 1.3 / 2.7).
`tools/gen_rdf_patch.py --verify-ids` re-checks every vanilla id used here against _work/vanilla_nemesis_xml
(run tools/dump_pandora_out.py --vanilla first).
"""
import os
import struct
import sys

# ----------------------------------------------------------------------------------------------- parameters
CODE = "rdf"
MOD_NAME = "Reactive Dodge Framework"
AUTHOR = "newrite"

EVENTS = ["RDF_DodgeStart", "RDF_DodgeStartB", "RDF_DodgeChain", "RDF_DodgeExit", "RDF_DodgeEntered", "RDF_DodgeLeft",
          "RDF_DodgeClipEnd"]
# Clip annotations (ANIMATION_CONTRACT.md). An annotation only reaches the plugin as an animation
# event when its text is a registered event name of the graph that plays the clip.
ANNOTATION_EVENTS = ["RDF_Windows", "RDF_IFrameStart", "RDF_IFrameEnd", "RDF_ChainOpen",
                     "RDF_AttackOpen", "RDF_BlockOpen", "RDF_ExitOpen"]
EVENTS += ANNOTATION_EVENTS
# (name, type, initial value)
VARIABLES = [("RDF_DodgeClip", "VARIABLE_TYPE_INT32", 0),
             ("RDF_DodgeSpeed", "VARIABLE_TYPE_REAL", 1.0),
             ("RDF_IsDodging", "VARIABLE_TYPE_BOOL", 0)]
# variables that are only bound if they already exist in the CURRENT output 0_master (checked by the caller)
OPTIONAL_BIND_VARS = ["DF_IsDodging"]          # TDM; not present in this modlist -> not created, not bound
EXISTING_VARS = {"bAnimationDriven"}           # must already exist in vanilla 0_master (index 81)

STATE_A = ("RDF_Dodge_A", 9701)
STATE_B = ("RDF_Dodge_B", 9702)
# A root wildcard into each state. The plugin alternates A and B for every new dodge, so a dodge pressed
# while the previous one is still blending out never targets the state that is being left (Havok drops
# that transition: the actor stays animation-driven by the dying blend and the dodge clip never plays).
START_EVENTS = [("RDF_DodgeStart", STATE_A), ("RDF_DodgeStartB", STATE_B)]
DEFAULT_STATE_ID = 14                          # RootBehaviorGraph state 'Default_State'
PLAYER_STAGGER_STATE_ID = 48                   # 'PlayerStagger_State'

# transition effects: name -> (duration s, flags, selfTransitionMode)
FX_IN = ("RDF_DodgeInBlend", 0.12, "FLAG_IGNORE_FROM_WORLD_FROM_MODEL", "SELF_TRANSITION_MODE_RESET")
FX_CHAIN = ("RDF_DodgeChainBlend", 0.10, "FLAG_IGNORE_FROM_WORLD_FROM_MODEL", "SELF_TRANSITION_MODE_RESET")
FX_OUT = ("RDF_DodgeOutBlend", 0.25, "FLAG_IGNORE_TO_WORLD_FROM_MODEL", "SELF_TRANSITION_MODE_BLEND")

CLIP_END_TRIGGER_TIME = -0.05                  # relative to end of clip
DIRS = ["f", "fr", "r", "br", "b", "bl", "l", "fl"]
ANIM_DIR = "Animations\\RDF"
CLIPS = ([("rdf_roll_%s" % d) for d in DIRS] + [("rdf_step_%s" % d) for d in DIRS])   # index order = RDF_DodgeClip

# "vanilla": mirror Stagger_State's return (4 conditional transitions that restore the Default_Behavior nested state);
# "simple": one unconditional transition per exit event to Default_State (sync variable restores nested state anyway).
RETURN_MODE = "vanilla"
EXIT_EVENTS = ["RDF_DodgeExit", "RDF_DodgeClipEnd", "IdleForceDefaultState"]   # IdleForceDefaultState exists in 0_master (496)

# vanilla 0_master (Nemesis ids)
V = dict(
    STRING="#0106", VALUES="#0107", DATA="#0108",
    ROOT_SM="#0340", ROOT_WILDCARD="#0341",
    ISACTIVE_TEMPLATE="#0129",                 # BSIsActiveModifier 'bAnimationDriven_IsActiveMod'
    FX_STAGGER_PLAYER="#0497",                 # 'FastBlendResetTransition' used by the StaggerPlayer transitions
    # Stagger_State's return conditions: (nestedFlag, toNested, condition id, expression)
    RETURN_CONDS=[(False, 0, "#0355", "(currentDefaultState < 10) || (bAttached == 0)"),
                  (True, 1, "#0354", "(currentDefaultState == 10) && (bAttached == 1)"),
                  (True, 2, "#0353", "(currentDefaultState == 11) && (bAttached == 1)"),
                  (True, 0, "#0350", "(currentDefaultState >= 12) && (bAttached == 1)")],
)


# ----------------------------------------------------------------------------------------------- helpers
def f2i(x):
    return struct.unpack("<i", struct.pack("<f", x))[0]


def fl(x):
    return "%.6f" % x


class Names:
    def __init__(self):
        self.n = 0

    def new(self):
        s = "#%s$%d" % (CODE, self.n)
        self.n += 1
        return s


def interval():
    return ('<hkobject><hkparam name="enterEventId">-1</hkparam><hkparam name="exitEventId">-1</hkparam>'
            '<hkparam name="enterTime">0.000000</hkparam><hkparam name="exitTime">0.000000</hkparam></hkobject>')


def ev(name):
    return "$eventID[%s]$" % name


def var(name):
    return "$variableID[%s]$" % name


def transition(effect, event, to_state, flags="FLAG_DISABLE_CONDITION", condition="null", to_nested=0):
    return ('<hkobject><hkparam name="triggerInterval">%s</hkparam><hkparam name="initiateInterval">%s</hkparam>'
            '<hkparam name="transition">%s</hkparam><hkparam name="condition">%s</hkparam>'
            '<hkparam name="eventId">%s</hkparam><hkparam name="toStateId">%d</hkparam>'
            '<hkparam name="fromNestedStateId">0</hkparam><hkparam name="toNestedStateId">%d</hkparam>'
            '<hkparam name="priority">0</hkparam><hkparam name="flags">%s</hkparam></hkobject>'
            % (interval(), interval(), effect, condition, ev(event) if not event.lstrip("-").isdigit() else event,
               to_state, to_nested, flags))


def node(name, cls, sig, body):
    return '<hkobject name="%s" class="%s" signature="%s">\n%s\n</hkobject>' % (name, cls, sig, body)


def p(name, val):
    return '<hkparam name="%s">%s</hkparam>' % (name, val)


def binding_set(name, bindings):
    """bindings = [(memberPath, variableName)]"""
    items = "".join('<hkobject>%s%s%s%s</hkobject>' % (p("memberPath", m), p("variableIndex", var(v)),
                                                      p("bitIndex", "-1"), p("bindingType", "BINDING_TYPE_VARIABLE"))
                    for m, v in bindings)
    return node(name, "hkbVariableBindingSet", "0x338ad4ff",
                '<hkparam name="bindings" numelements="%d">%s</hkparam>%s' % (len(bindings), items,
                                                                              p("indexOfBindingToEnable", "-1")))


def effect_node(name, spec, bind_name_ptr="null"):
    nm, dur, flags, selfmode = spec
    return node(name, "hkbBlendingTransitionEffect", "0xfd8584fe", "\n".join([
        p("variableBindingSet", "null"), p("userData", "0"), p("name", nm), p("selfTransitionMode", selfmode),
        p("eventMode", "EVENT_MODE_DEFAULT"), p("duration", fl(dur)), p("toGeneratorStartTimeFraction", "0.000000"),
        p("flags", flags), p("endMode", "END_MODE_NONE"), p("blendCurve", "BLEND_CURVE_SMOOTH")]))


def event_array(name, ev_name):
    return node(name, "hkbStateMachineEventPropertyArray", "0xb07b4388",
                '<hkparam name="events" numelements="1"><hkobject>%s%s</hkobject></hkparam>'
                % (p("id", ev(ev_name)), p("payload", "null")))


def build_state(nm, state_id, other_id, N, fx, bind_extra):
    """returns (state node name, [xml nodes])."""
    out = []
    st = N.new()
    enter, exit_, trans, modgen, isact, isact_bs, sel, sel_bs = (N.new() for _ in range(8))
    out.append(event_array(enter, "RDF_DodgeEntered"))
    out.append(event_array(exit_, "RDF_DodgeLeft"))
    # --- transitions
    ts = []
    ts.append(transition(fx["chain"], "RDF_DodgeChain", other_id))
    for e in EXIT_EVENTS:
        if RETURN_MODE == "vanilla":
            for nested_flag, to_nested, cond, _expr in V["RETURN_CONDS"]:
                flags = "FLAG_TO_NESTED_STATE_ID_IS_VALID" if nested_flag else "0"
                ts.append(transition(fx["out"], e, DEFAULT_STATE_ID, flags, cond, to_nested))
        else:
            ts.append(transition(fx["out"], e, DEFAULT_STATE_ID))
    ts.append(transition(V["FX_STAGGER_PLAYER"], "StaggerPlayer", PLAYER_STAGGER_STATE_ID))
    out.append(node(trans, "hkbStateMachineTransitionInfoArray", "0xe397b11e",
                    '<hkparam name="transitions" numelements="%d">%s</hkparam>' % (len(ts), "".join(ts))))
    # --- state
    out.append(node(st, "hkbStateMachineStateInfo", "0xed7f9d0", "\n".join([
        p("variableBindingSet", "null"), '<hkparam name="listeners" numelements="0"></hkparam>',
        p("enterNotifyEvents", enter), p("exitNotifyEvents", exit_), p("transitions", trans), p("generator", modgen),
        p("name", nm), p("stateId", state_id), p("probability", "1.000000"), p("enable", "true")])))
    # --- generator: ModifierGenerator { BSIsActiveModifier, ManualSelector{16 clips} }
    out.append(node(modgen, "hkbModifierGenerator", "0x1f81fae6", "\n".join([
        p("variableBindingSet", "null"), p("userData", "1"), p("name", nm + "_MG"), p("modifier", isact),
        p("generator", sel)])))
    binds = [("bIsActive0", "bAnimationDriven"), ("bIsActive1", "RDF_IsDodging")] + bind_extra
    out.append(binding_set(isact_bs, binds))
    flags = "".join(p("bIsActive%d" % i, "false") + p("bInvertActive%d" % i, "false") for i in range(5))
    out.append(node(isact, "BSIsActiveModifier", "0xb0fde45a", "\n".join([
        p("variableBindingSet", isact_bs), p("userData", "2"), p("name", nm + "_IsActiveMod"), p("enable", "true"),
        flags])))
    clip_names = []
    for ci, cname in enumerate(CLIPS):
        c, trg, cbs = N.new(), N.new(), N.new()
        clip_names.append(c)
        out.append(binding_set(cbs, [("playbackSpeed", "RDF_DodgeSpeed")]))
        out.append(node(trg, "hkbClipTriggerArray", "0x59c23a0f",
                        '<hkparam name="triggers" numelements="1"><hkobject>%s'
                        '<hkparam name="event"><hkobject>%s%s</hkobject></hkparam>%s%s%s</hkobject></hkparam>'
                        % (p("localTime", fl(CLIP_END_TRIGGER_TIME)), p("id", ev("RDF_DodgeClipEnd")),
                           p("payload", "null"), p("relativeToEndOfClip", "true"), p("acyclic", "false"),
                           p("isAnnotation", "false"))))
        out.append(node(c, "hkbClipGenerator", "0x333b85b9", "\n".join([
            p("variableBindingSet", cbs), p("userData", "0"), p("name", "%s_%s" % (nm, cname)),
            p("animationName", "%s\\%s.hkx" % (ANIM_DIR, cname)), p("triggers", trg),
            p("cropStartAmountLocalTime", "0.000000"), p("cropEndAmountLocalTime", "0.000000"),
            p("startTime", "0.000000"), p("playbackSpeed", "1.000000"), p("enforcedDuration", "0.000000"),
            p("userControlledTimeFraction", "0.000000"), p("animationBindingIndex", "-1"),
            p("mode", "MODE_SINGLE_PLAY"), p("flags", "0")])))
    out.append(binding_set(sel_bs, [("selectedGeneratorIndex", "RDF_DodgeClip")]))
    out.append(node(sel, "hkbManualSelectorGenerator", "0xd932fab8", "\n".join([
        p("variableBindingSet", sel_bs), p("userData", "0"), p("name", nm + "_ClipSelector"),
        '<hkparam name="generators" numelements="%d">%s</hkparam>' % (len(clip_names), " ".join(clip_names)),
        p("selectedGeneratorIndex", "0"), p("currentGeneratorIndex", "0")])))
    return st, out


# ----------------------------------------------------------------------------------------------- generation
def gen_master(existing_vars):
    N = Names()
    x = ["<patch>"]
    # events
    for e in EVENTS:
        x.append('  <append path="%s/eventNames"><hkcstring>%s</hkcstring></append>' % (V["STRING"], e))
        x.append('  <append path="%s/eventInfos"><hkobject><hkparam name="flags">0</hkparam></hkobject></append>'
                 % V["DATA"])
    # variables: three parallel lists
    for name, typ, init in VARIABLES:
        val = f2i(init) if typ == "VARIABLE_TYPE_REAL" else int(init)
        x.append('  <append path="%s/variableNames"><hkcstring>%s</hkcstring></append>' % (V["STRING"], name))
        x.append('  <append path="%s/wordVariableValues"><hkobject><hkparam name="value">%d</hkparam></hkobject></append>'
                 % (V["VALUES"], val))
        x.append('  <append path="%s/variableInfos"><hkobject><hkparam name="role"><hkobject>'
                 '<hkparam name="role">ROLE_DEFAULT</hkparam><hkparam name="flags">0</hkparam></hkobject></hkparam>'
                 '<hkparam name="type">%s</hkparam></hkobject></append>' % (V["DATA"], typ))
    bind_extra = []
    for i, v in enumerate(OPTIONAL_BIND_VARS):
        if v in existing_vars:
            bind_extra.append(("bIsActive%d" % (2 + i), v))
    # shared effect nodes
    fx_nodes = {}
    new_nodes = []
    for key, spec in (("in", FX_IN), ("chain", FX_CHAIN), ("out", FX_OUT)):
        nm = N.new()
        fx_nodes[key] = nm
        new_nodes.append(effect_node(nm, spec))
    st_a, nodes_a = build_state(STATE_A[0], STATE_A[1], STATE_B[1], N, fx_nodes, bind_extra)
    st_b, nodes_b = build_state(STATE_B[0], STATE_B[1], STATE_A[1], N, fx_nodes, bind_extra)
    new_nodes += nodes_a + nodes_b
    x.append('  <insert path="">')
    x += ["    " + n.replace("\n", "\n    ") for n in new_nodes]
    x.append("  </insert>")
    x.append('  <append path="%s/states">%s</append>' % (V["ROOT_SM"], st_a))
    x.append('  <append path="%s/states">%s</append>' % (V["ROOT_SM"], st_b))
    for event, (_, state_id) in START_EVENTS:
        x.append('  <append path="%s/transitions">%s</append>'
                 % (V["ROOT_WILDCARD"], transition(fx_nodes["in"], event, state_id,
                                                   "FLAG_IS_LOCAL_WILDCARD|FLAG_DISABLE_CONDITION")))
    x.append("</patch>")
    return "\n".join(x) + "\n", N.n, bind_extra


def gen_character():
    x = ["<patch>"]
    for c in CLIPS:
        x.append('  <append path="#0029/animationNames"><hkcstring>%s\\%s.hkx</hkcstring></append>' % (ANIM_DIR, c))
    x.append("</patch>")
    return "\n".join(x) + "\n"


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def verify_ids(root):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import hkxcheck as H
    g = H.Graph(os.path.join(root, "_work", "vanilla_nemesis_xml", "0_master_nem.xml"))
    ok = True

    def chk(cond, msg):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + msg)
        ok &= bool(cond)
    chk(g.objs[V["STRING"]].cls == "hkbBehaviorGraphStringData", V["STRING"] + " StringData")
    chk(g.objs[V["VALUES"]].cls == "hkbVariableValueSet", V["VALUES"] + " VariableValueSet")
    chk(g.objs[V["DATA"]].cls == "hkbBehaviorGraphData", V["DATA"] + " GraphData")
    chk(g.objs[V["ROOT_SM"]].text("name") == "RootBehaviorGraph", V["ROOT_SM"] + " RootBehaviorGraph")
    chk(g.objs[V["ROOT_SM"]].text("wildcardTransitions") == V["ROOT_WILDCARD"], V["ROOT_WILDCARD"] + " is its wildcard array")
    chk(g.objs[V["FX_STAGGER_PLAYER"]].text("name") == "FastBlendResetTransition", V["FX_STAGGER_PLAYER"])
    for _f, _n, cid, expr in V["RETURN_CONDS"]:
        chk(g.objs[cid].text("expression") == expr, cid + " " + expr)
    sm = g.objs[V["ROOT_SM"]]
    sids = {g.objs[s].intval("stateId"): g.objs[s].text("name") for s in H.sm_states(g, sm)}
    chk(sids.get(DEFAULT_STATE_ID) == "Default_State", "stateId 14 Default_State")
    chk(sids.get(PLAYER_STAGGER_STATE_ID) == "PlayerStagger_State", "stateId 48 PlayerStagger_State")
    chk(STATE_A[1] not in sids and STATE_B[1] not in sids, "9701/9702 free in vanilla root SM")
    for v in EXISTING_VARS:
        chk(v in g.var_index, "variable %s exists" % v)
    for e in ["IdleForceDefaultState", "StaggerPlayer"]:
        chk(e in g.ev_index, "event %s exists" % e)
    return ok


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--verify-ids" in sys.argv:
        return 0 if verify_ids(root) else 1
    out = args[0] if args else os.path.join(root, "behavior", "Pandora_Engine", "mod", CODE)
    # DF_IsDodging is NOT in this modlist's 0_master -> do not bind. Pass --with-df to force when it exists.
    existing = set(OPTIONAL_BIND_VARS) if "--with-df" in sys.argv else set()
    master, n_nodes, binds = gen_master(existing)
    write(os.path.join(out, "info.xml"),
          '<?xml version="1.0" encoding="utf-8"?>\n<mod code="%s">\n  <name>%s</name>\n  <author>%s</author>\n'
          '  <site>null</site>\n</mod>\n' % (CODE, MOD_NAME, AUTHOR))
    write(os.path.join(out, "patches", "0_master.xml"), master)
    ch = gen_character()
    write(os.path.join(out, "patches", "defaultmale_character.xml"), ch)
    write(os.path.join(out, "patches", "defaultfemale_character.xml"), ch)
    print("wrote %s: %d new nodes, extra bindings %s" % (out, n_nodes, binds))
    return 0


if __name__ == "__main__":
    sys.exit(main())

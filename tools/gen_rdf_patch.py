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

EVENTS = ["RDF_DodgeStart", "RDF_DodgeStartB", "RDF_DodgeChain", "RDF_DodgeExit", "RDF_DodgeEntered", "RDF_DodgeLeft"]
# Clip annotations (ANIMATION_CONTRACT.md). An annotation only reaches the plugin as an animation
# event when its text is a registered event name of the graph that plays the clip.
ANNOTATION_EVENTS = ["RDF_Windows", "RDF_IFrameStart", "RDF_IFrameEnd", "RDF_ChainOpen",
                     "RDF_AttackOpen", "RDF_BlockOpen", "RDF_ExitOpen"]
EVENTS += ANNOTATION_EVENTS
# Sent by the plugin on a perfect dodge (to the player and to the attacker); registered so behaviors, OAR and other
# plugins' graph-event sinks receive them (NotifyAnimationGraph drops a name the graph does not know).
EVENTS += ["RDF_PerfectDodge", "RDF_PerfectDodged"]
# (name, type, initial value)
VARIABLES = [("RDF_DodgeClip", "VARIABLE_TYPE_INT32", 0),
             ("RDF_DodgeSpeed", "VARIABLE_TYPE_REAL", 1.0),
             ("RDF_IsDodging", "VARIABLE_TYPE_BOOL", 0),
             ("RDF_ArmBlend", "VARIABLE_TYPE_REAL", 0.0)]    # 0 = arms follow the dodge clip, 1 = arms hold the weapon stance
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

DIRS = ["f", "fr", "r", "br", "b", "bl", "l", "fl"]
ANIM_DIR = "Animations\\RDF"
CLIPS = ([("rdf_roll_%s" % d) for d in DIRS] + [("rdf_step_%s" % d) for d in DIRS])   # index order = RDF_DodgeClip

# "vanilla": mirror Stagger_State's return (4 conditional transitions that restore the Default_Behavior nested state);
# "simple": one unconditional transition per exit event to Default_State (sync variable restores nested state anyway).
RETURN_MODE = "vanilla"
# No clip-end exit: a clip played for the first time in a session is still streaming in, has zero length,
# and a relative-to-end trigger fires at once, which dropped the dodge on entry. The plugin ends every
# dodge itself (RDF_DodgeExit); IdleForceDefaultState is the engine's own safety exit.
EXIT_EVENTS = ["RDF_DodgeExit", "IdleForceDefaultState"]   # IdleForceDefaultState exists in 0_master (496)


# ------------------------------------------------------------------------- arm stance layer (see BEHAVIOR_INTEGRATION.md 5)
# State generator = ModifierGenerator{ BSIsActiveModifier,
#   Blender(flags 0)  child0 = dodge clip selector       mask = NOT(arms)   weight 1  worldFromModel 1
#                     child1 = ArmLayer                  mask = arms        weight 1  worldFromModel 0
#   ArmLayer = Blender(PARAMETRIC, flags 16, blendParameter <- RDF_ArmBlend)
#                     p0 (weight 0) = a second dodge clip selector        p1 (weight 1) = StanceBlender
#   StanceBlender = Blender(flags 0)  right selector (iRightHandType) mask RightArm / left selector (iLeftHandType) mask LeftArm }
# The masks of the outer blender are complementary (vanilla pattern, e.g. 1HM_Readied blends), so the result does not
# depend on how Havok normalises weights; the parametric inner blender is a plain lerp (vanilla 'Tor_1stP_Locomotion_Blend').
BLEND_FLAGS_PLAIN = 0
BLEND_FLAGS_PARAMETRIC = 16
ARM_BLEND_VAR = "RDF_ArmBlend"
HAND_VARS = {"R": "iRightHandType", "L": "iLeftHandType"}
# graph-local indices into 0_master's characterPropertyNames (verified by --verify-ids against the vanilla dump)
CHAR_PROP = {"R": ("RightArm", 8), "L": ("LeftArm", 5)}
N_BONES = 126                                  # XPMSE skeleton (99 vanilla bones + 27 extra); masks are full length
# Bone indices of the vanilla character-property masks RightArm / LeftArm in the CURRENT character files
# (XPMSE-extended: LeftArm also lists the extra bones 99..125). validate_rdf.py re-reads the real masks.
RIGHT_ARM_BONES = [30, 31, 32, 39, 41, 43, 45, 48, 56, 57, 58, 59] + list(range(82, 97))
LEFT_ARM_BONES = [27, 28, 29, 38, 40, 42, 44, 47, 52, 53, 54, 55] + list(range(67, 82)) + list(range(99, 126))
STANCE_CLIP_FILES = {k: "\\".join(v.split("/")) for k, v in {
    "H2H": "Animations/H2H_Idle.hkx", "1HM": "Animations/1HM_Idle.hkx", "DW": "Animations/DW1HM1HMIdle.hkx",
    "2HM": "Animations/2HM_Idle.HKX", "2HW": "Animations/2HW_Idle.HKX", "BOW": "Animations/Bow_IdleHeld.hkx",
    "STAFFR": "Animations/StaffRight_Idle.hkx", "STAFFL": "Animations/Staff_Idle.hkx",
    "MRH": "Animations/MRh_Idle.hkx", "MLH": "Animations/MLh_Idle.hkx", "TORCH": "Animations/Tor_1HMPose.hkx",
    "XBOW": "Animations/DLC01/CrossBow_IdleHeld.hkx", "MT": "Animations/MT_Idle.hkx"}.items()}
# hand-type value -> stance clip key. Values from how vanilla compares them (1hm_locomotion Melee_Direction_Behavior,
# 1hm_behavior Weap_Idle_Behavior, magicbehavior MagicReady_*): 0 none/H2H, 1 sword, 2 dagger, 3 axe, 4 mace, 5 2H sword,
# 6 2H axe/hammer, 7 bow, 8 staff, 9 spell, 10 shield, 11 torch, 12 crossbow.
# "1H*" in the right table = nested selector on iLeftHandType (vanilla 'Weap_1HM_Idle_MSG': left 1..4 -> dual-wield pose).
STANCE_RIGHT = ["H2H", "1H", "1H", "1H", "1H", "2HM", "2HW", "BOW", "STAFFR", "MRH", "1HM", "1HM", "XBOW"]
STANCE_LEFT = ["1HM", "DW", "DW", "DW", "DW", "2HM", "2HW", "BOW", "STAFFL", "MLH", "1HM", "TORCH", "XBOW"]
STANCE_NESTED_LEFT = ["1HM", "DW", "DW", "DW", "DW", "1HM", "1HM", "1HM", "1HM", "1HM", "1HM", "1HM", "1HM"]

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


# ------------------------------------------------------------------------- first person (firstperson~0_master)
# The 1P graph (project `firstperson`, Pandora file name `firstperson~0_master`) mirrors the 3P 0_master: same event/variable
# tables (1217 events / 230 variables), RootBehaviorGraph with the same stateIds (14 Default_State, 48 PlayerStagger_State,
# 7 Stagger_State), but its own Nemesis ids. A state plays Bethesda's own 1P dodge clip on torso, head and camera, and on the
# arms a parametric blend (RDF_ArmBlend: 0 clip, 1 stance) with the 1P weapon-ready idle of the current hand types.
# The dodge clips are vanilla `Animations\DodgeLeft/Right/Back.hkx`, already in the vanilla FirstPerson character (no copies,
# and OAR replacers such as Sigma - Magic apply). Forward has no 1P dodge: the body keeps the neutral idle (see FP_DODGE_CLIPS).
# TUDM's 1P roll_*/sidestep_* are 16 copies of that same roll, so they add nothing.
V_FP = dict(
    STRING="#0095", VALUES="#0096", DATA="#0097",
    ROOT_SM="#0167", ROOT_WILDCARD="#0168",
    FX_STAGGER_PLAYER="#0245",                 # 'FastBlendResetTransition' on the 1P StaggerPlayer transitions
    RETURN_CONDS=[(False, 0, "#0181", "(currentDefaultState < 10) || (bAttached == 0)"),
                  (True, 1, "#0180", "(currentDefaultState == 10) && (bAttached == 1)"),
                  (True, 2, "#0179", "(currentDefaultState == 11) && (bAttached == 1)"),
                  (True, 0, "#0176", "(currentDefaultState >= 12) && (bAttached == 1)")],
)
FP_PATCH_NAME = "firstperson~0_master.xml"
# 1P skeleton (vanilla _1stperson/characterassets/skeletonfirst.hkx: 99 bones, same order as the vanilla 3P one; no mod in
# this list replaces it): clavicles, arms, twists, hands, fingers, pauldrons, magic nodes and the hand/weapon/shield nodes.
N_BONES_FP = 99
FP_ARM_BONES = [27, 28, 29, 30, 31, 32, 38, 39, 40, 41, 42, 43, 44, 45, 47, 48] + list(range(52, 60)) + list(range(67, 97))
# Forward has no 1P dodge clip. The vanilla 1P sneak roll dropped the camera far below the arms (which hold the stance),
# so the arms left the view; forward keeps the neutral 1P idle on the body and the plugin alone moves the player.
FP_DODGE_CLIPS = {"f": "MT_Idle", "fr": "DodgeRight", "r": "DodgeRight", "br": "DodgeRight",
                  "b": "DodgeBack", "bl": "DodgeLeft", "l": "DodgeLeft", "fl": "DodgeLeft"}
# Outer 1P chooser: hkbStateMachine startStateId <- currentDefaultState (the vanilla sync variable that holds the stateId of
# Default_Behavior: 0 MT/sheathed, 1 weapon readied, 2 magic readied, 10/11 equip, 12/13 unequip, 14 kill move, 20 paired).
# Weapon pose only while a weapon/spell is out; sheathed (or being sheathed) -> neutral 1P idle `MT_Idle`.
FP_DRAWN = {0: "MT", 1: "STANCE", 2: "STANCE", 10: "STANCE", 11: "STANCE", 12: "MT", 13: "MT", 14: "MT", 20: "MT"}
FP_DRAWN_VAR = "currentDefaultState"
# 1P stance chooser: hkbStateMachine startStateId <- iRightHandType, stateId = hand-type value 0..12. Entries "L0"/"L1" are
# nested machines on iLeftHandType (vanilla `1HM_1stP_BodyBehavior`/`WeapIdleMSG`: left 1-4 = dual wield pose, 8-11 = 1HM pose).
# State machines instead of ManualSelectors: the SE selector has no change effect and shows a one-frame pose step when it
# reactivates on a different child (3P pose trace), a state machine applies the bound start state at activation.
FP_STANCE_RIGHT = ["L0", "L1", "L1", "L1", "L1", "2HM", "2HW", "BOW", "STAFFR", "MRH", "1HM", "1HM", "XBOW"]
FP_NESTED = {
    "L0": ["H2H", "H2H", "H2H", "H2H", "H2H", "H2H", "H2H", "H2H", "H2H", "H2H", "1HM", "H2H", "H2H"],   # right hand empty
    "L1": ["1HM", "DW", "DW", "DW", "DW", "2HM", "2HW", "BOW", "1HM", "1HM", "1HM", "1HM", "1HM"],       # right 1H weapon
}


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


def event_array(name, *ev_names):
    items = "".join("<hkobject>%s%s</hkobject>" % (p("id", ev(e)), p("payload", "null")) for e in ev_names)
    return node(name, "hkbStateMachineEventPropertyArray", "0xb07b4388",
                '<hkparam name="events" numelements="%d">%s</hkparam>' % (len(ev_names), items))


def char_prop_binding_set(name, member, index):
    return node(name, "hkbVariableBindingSet", "0x338ad4ff",
                '<hkparam name="bindings" numelements="1"><hkobject>%s%s%s%s</hkobject></hkparam>%s'
                % (p("memberPath", member), p("variableIndex", index), p("bitIndex", "-1"),
                   p("bindingType", "BINDING_TYPE_CHARACTER_PROPERTY"), p("indexOfBindingToEnable", "-1")))


def bone_weight_array(name, weights):
    return node(name, "hkbBoneWeightArray", "0xcd902b77",
                p("variableBindingSet", "null") + '\n<hkparam name="boneWeights" numelements="%d">%s</hkparam>'
                % (len(weights), " ".join(fl(w) for w in weights)))


def blender_child(name, generator, bone_weights, weight, wfm, bindset="null"):
    return node(name, "hkbBlenderGeneratorChild", "0xe2b384b0", "\n".join([
        p("variableBindingSet", bindset), p("generator", generator), p("boneWeights", bone_weights),
        p("weight", fl(weight)), p("worldFromModelWeight", fl(wfm))]))


_names = {}


def named(ptr, text):
    _names[ptr] = text
    return ptr


def blender(ptr, children, flags, bindset="null"):
    return node(ptr, "hkbBlenderGenerator", "0x22df7147", "\n".join([
        p("variableBindingSet", bindset), p("userData", "0"), p("name", _names.get(ptr, ptr)),
        p("referencePoseWeightThreshold", "0.000000"), p("blendParameter", "1.000000"),
        p("minCyclicBlendParameter", "0.000000"), p("maxCyclicBlendParameter", "1.000000"),
        p("indexOfSyncMasterChild", "-1"), p("flags", flags), p("subtractLastChild", "false"),
        '<hkparam name="children" numelements="%d">%s</hkparam>' % (len(children), " ".join(children))]))


def selector_node(N, out, label, var_name, entries):
    ptr, bs = N.new(), N.new()
    out.append(binding_set(bs, [("selectedGeneratorIndex", var_name)]))
    out.append(node(ptr, "hkbManualSelectorGenerator", "0xd932fab8", "\n".join([
        p("variableBindingSet", bs), p("userData", "0"), p("name", label),
        '<hkparam name="generators" numelements="%d">%s</hkparam>' % (len(entries), " ".join(entries)),
        p("selectedGeneratorIndex", "0"), p("currentGeneratorIndex", "0")])))
    return ptr


def clip_node(N, out, label, anim, mode, speed_var=None):
    c = N.new()
    bs = "null"
    if speed_var:
        bs = N.new()
        out.append(binding_set(bs, [("playbackSpeed", speed_var)]))
    out.append(node(c, "hkbClipGenerator", "0x333b85b9", "\n".join([
        p("variableBindingSet", bs), p("userData", "0"), p("name", label), p("animationName", anim),
        p("triggers", "null"), p("cropStartAmountLocalTime", "0.000000"), p("cropEndAmountLocalTime", "0.000000"),
        p("startTime", "0.000000"), p("playbackSpeed", "1.000000"), p("enforcedDuration", "0.000000"),
        p("userControlledTimeFraction", "0.000000"), p("animationBindingIndex", "-1"), p("mode", mode),
        p("flags", "0")])))
    return c


def fp_clip_path(cname):
    """1P animation for a CLIPS name: rdf_roll_fr -> Animations\\DodgeRight.hkx."""
    return "Animations\\%s.hkx" % FP_DODGE_CLIPS[cname.rsplit("_", 1)[1]]


def dodge_selector(N, out, label, path=None):
    """Dodge clip chooser: hkbStateMachine, startStateId <- RDF_DodgeClip, one state per clip (stateId = clip index).

    Not an hkbManualSelectorGenerator: the SE selector (no change effect) reactivates on its previous child and
    only switches to the bound index a frame later, unblended - a one-frame whole-body pose step whenever the state
    played a different clip last time (in-game pose trace). A state machine picks its start state from the binding
    at activation, like TUDM's *DodgeStyle machines and vanilla `startStateId <- iRightHandType` machines."""
    sm, bs = N.new(), N.new()
    states = []
    for i, cname in enumerate(CLIPS):
        anim = path(cname) if path else "%s\\%s.hkx" % (ANIM_DIR, cname)
        clip = clip_node(N, out, "%s_%s" % (label, cname), anim, "MODE_SINGLE_PLAY", "RDF_DodgeSpeed")
        st = N.new()
        out.append(node(st, "hkbStateMachineStateInfo", "0xed7f9d0", "\n".join([
            p("variableBindingSet", "null"), '<hkparam name="listeners" numelements="0"></hkparam>',
            p("enterNotifyEvents", "null"), p("exitNotifyEvents", "null"), p("transitions", "null"),
            p("generator", clip), p("name", "%s_S%d_%s" % (label, i, cname)), p("stateId", i),
            p("probability", "1.000000"), p("enable", "true")])))
        states.append(st)
    out.append(binding_set(bs, [("startStateId", "RDF_DodgeClip")]))
    out.append(node(sm, "hkbStateMachine", "0x816c1dcb", "\n".join([
        p("variableBindingSet", bs), p("userData", "0"), p("name", label + "_ClipSM"),
        '<hkparam name="eventToSendWhenStateOrTransitionChanges"><hkobject>%s%s</hkobject></hkparam>'
        % (p("id", "-1"), p("payload", "null")),
        p("startStateChooser", "null"), p("startStateId", "0"), p("returnToPreviousStateEventId", "-1"),
        p("randomTransitionEventId", "-1"), p("transitionToNextHigherStateEventId", "-1"),
        p("transitionToNextLowerStateEventId", "-1"), p("syncVariableIndex", "-1"), p("wrapAroundStateId", "false"),
        p("maxSimultaneousTransitions", "32"), p("startStateMode", "START_STATE_MODE_DEFAULT"),
        p("selfTransitionMode", "SELF_TRANSITION_MODE_NO_TRANSITION"),
        '<hkparam name="states" numelements="%d">%s</hkparam>' % (len(states), " ".join(states)),
        p("wildcardTransitions", "null")])))
    return sm


def stance_selector(N, out, label, side):
    """side 'R'/'L': ManualSelector(iRightHandType / iLeftHandType) over the 13 hand-type values of looping stance clips."""
    clips = {}

    def clip(key):
        if key not in clips:
            clips[key] = clip_node(N, out, "%s_%s" % (label, key), STANCE_CLIP_FILES[key], "MODE_LOOPING")
        return clips[key]
    table = STANCE_RIGHT if side == "R" else STANCE_LEFT
    nested = None
    entries = []
    for v, key in enumerate(table):
        if key == "1H":   # right hand 1H weapon: nested selector on iLeftHandType (dual wield vs single)
            if nested is None:
                nested = selector_node(N, out, label + "_1H_LeftSel", HAND_VARS["L"],
                                       [clip(k) for k in STANCE_NESTED_LEFT])
            entries.append(nested)
        else:
            entries.append(clip(key))
    return selector_node(N, out, label + "_Sel", HAND_VARS[side], entries)


def chooser_sm(N, out, label, var_name, gens, ids=None):
    """hkbStateMachine, startStateId <- var_name, one state per generator, stateId = index (no transitions)."""
    sm, bs = N.new(), N.new()
    states = []
    ids = ids if ids is not None else list(range(len(gens)))
    for i, g_ in zip(ids, gens):
        st = N.new()
        out.append(node(st, "hkbStateMachineStateInfo", "0xed7f9d0", "\n".join([
            p("variableBindingSet", "null"), '<hkparam name="listeners" numelements="0"></hkparam>',
            p("enterNotifyEvents", "null"), p("exitNotifyEvents", "null"), p("transitions", "null"),
            p("generator", g_), p("name", "%s_S%d" % (label, i)), p("stateId", i),
            p("probability", "1.000000"), p("enable", "true")])))
        states.append(st)
    out.append(binding_set(bs, [("startStateId", var_name)]))
    out.append(node(sm, "hkbStateMachine", "0x816c1dcb", "\n".join([
        p("variableBindingSet", bs), p("userData", "0"), p("name", label),
        '<hkparam name="eventToSendWhenStateOrTransitionChanges"><hkobject>%s%s</hkobject></hkparam>'
        % (p("id", "-1"), p("payload", "null")),
        p("startStateChooser", "null"), p("startStateId", "0"), p("returnToPreviousStateEventId", "-1"),
        p("randomTransitionEventId", "-1"), p("transitionToNextHigherStateEventId", "-1"),
        p("transitionToNextLowerStateEventId", "-1"), p("syncVariableIndex", "-1"), p("wrapAroundStateId", "false"),
        p("maxSimultaneousTransitions", "32"), p("startStateMode", "START_STATE_MODE_DEFAULT"),
        p("selfTransitionMode", "SELF_TRANSITION_MODE_NO_TRANSITION"),
        '<hkparam name="states" numelements="%d">%s</hkparam>' % (len(states), " ".join(states)),
        p("wildcardTransitions", "null")])))
    return sm


def stance_chooser_fp(N, out, label):
    clips = {}

    def clip(key):
        if key not in clips:
            clips[key] = clip_node(N, out, "%s_%s" % (label, key), STANCE_CLIP_FILES[key], "MODE_LOOPING")
        return clips[key]
    nested = {}
    entries = []
    for key in FP_STANCE_RIGHT:
        if key in FP_NESTED:
            if key not in nested:
                nested[key] = chooser_sm(N, out, "%s_Left_%s" % (label, key), HAND_VARS["L"],
                                         [clip(k) for k in FP_NESTED[key]])
            entries.append(nested[key])
        else:
            entries.append(clip(key))
    stance = chooser_sm(N, out, label + "_StanceSM", HAND_VARS["R"], entries)
    mt = clip("MT")
    ids = sorted(FP_DRAWN)
    return chooser_sm(N, out, label + "_DrawnSM", FP_DRAWN_VAR, [stance if FP_DRAWN[i] == "STANCE" else mt for i in ids], ids)


def build_state(nm, state_id, other_id, N, fx, bind_extra, masks, v=None, fp=False):
    """returns (state node name, [xml nodes])."""
    v = v or V
    out = []
    st = N.new()
    enter, exit_, trans, modgen, isact, isact_bs = (N.new() for _ in range(6))
    # InterruptCast: the vanilla event every state that takes over from casting sends on enter (Stagger_State,
    # jump/land, swim, ragdoll...); the engine's InterruptCastHandler then calls Actor::InterruptCast (ID 37808), which
    # ends each caster's charge. Without it a dodge out of a cast leaves the caster charging after the return.
    out.append(event_array(enter, "RDF_DodgeEntered", "InterruptCast"))
    out.append(event_array(exit_, "RDF_DodgeLeft"))
    # --- transitions
    ts = []
    ts.append(transition(fx["chain"], "RDF_DodgeChain", other_id))
    for e in EXIT_EVENTS:
        if RETURN_MODE == "vanilla":
            for nested_flag, to_nested, cond, _expr in v["RETURN_CONDS"]:
                flags = "FLAG_TO_NESTED_STATE_ID_IS_VALID" if nested_flag else "0"
                ts.append(transition(fx["out"], e, DEFAULT_STATE_ID, flags, cond, to_nested))
        else:
            ts.append(transition(fx["out"], e, DEFAULT_STATE_ID))
    ts.append(transition(v["FX_STAGGER_PLAYER"], "StaggerPlayer", PLAYER_STAGGER_STATE_ID))
    out.append(node(trans, "hkbStateMachineTransitionInfoArray", "0xe397b11e",
                    '<hkparam name="transitions" numelements="%d">%s</hkparam>' % (len(ts), "".join(ts))))
    # --- state
    out.append(node(st, "hkbStateMachineStateInfo", "0xed7f9d0", "\n".join([
        p("variableBindingSet", "null"), '<hkparam name="listeners" numelements="0"></hkparam>',
        p("enterNotifyEvents", enter), p("exitNotifyEvents", exit_), p("transitions", trans), p("generator", modgen),
        p("name", nm), p("stateId", state_id), p("probability", "1.000000"), p("enable", "true")])))
    # --- ModifierGenerator { BSIsActiveModifier, Blender }
    if fp:   # first person: ModifierGenerator{ BSIsActiveModifier, Blend{ body: clip, arms: Blend(RDF_ArmBlend){ clip, stance } } }
        # Same layout as 3P: the clip moves torso, head and camera (the dodge's impact), the arms take the stance share,
        # so a two-handed grip stays on the weapon instead of following the one-handed pose of Bethesda's 1P clips.
        gen_ptr, c_main, c_arm, arm, arm_bs, p0, p1 = (N.new() for _ in range(7))
        named(gen_ptr, nm + "_FPArmStanceBlend")
        named(arm, nm + "_FPArmLayer")
        main = dodge_selector(N, out, nm + "_FP", fp_clip_path)
        arm_dodge = dodge_selector(N, out, nm + "_FPArmDodge", fp_clip_path)
        stance = stance_chooser_fp(N, out, nm)
        out.append(blender_child(c_main, main, masks["not_arms"], 1.0, 0.0))
        out.append(blender_child(c_arm, arm, masks["arms"], 1.0, 0.0))
        out.append(blender(gen_ptr, [c_main, c_arm], BLEND_FLAGS_PLAIN))
        out.append(binding_set(arm_bs, [("blendParameter", ARM_BLEND_VAR)]))
        out.append(blender_child(p0, arm_dodge, "null", 0.0, 0.0))
        out.append(blender_child(p1, stance, "null", 1.0, 0.0))
        out.append(blender(arm, [p0, p1], BLEND_FLAGS_PARAMETRIC, arm_bs))
        out.append(node(modgen, "hkbModifierGenerator", "0x1f81fae6", "\n".join([
            p("variableBindingSet", "null"), p("userData", "1"), p("name", nm + "_MG"), p("modifier", isact),
            p("generator", gen_ptr)])))
        out.append(binding_set(isact_bs, [("bIsActive0", "bAnimationDriven"), ("bIsActive1", "RDF_IsDodging")]))
        fl_ = "".join(p("bIsActive%d" % i, "false") + p("bInvertActive%d" % i, "false") for i in range(5))
        out.append(node(isact, "BSIsActiveModifier", "0xb0fde45a", "\n".join([
            p("variableBindingSet", isact_bs), p("userData", "2"), p("name", nm + "_IsActiveMod"), p("enable", "true"),
            fl_])))
        return st, out
    outer, outer_c0, outer_c1, arm, arm_bs, arm_p0, arm_p1, stance, st_c0, st_c1 = (N.new() for _ in range(10))
    named(outer, nm + "_ArmStanceBlend")
    named(arm, nm + "_ArmLayer")
    named(stance, nm + "_StanceBlend")
    out.append(node(modgen, "hkbModifierGenerator", "0x1f81fae6", "\n".join([
        p("variableBindingSet", "null"), p("userData", "1"), p("name", nm + "_MG"), p("modifier", isact),
        p("generator", outer)])))
    binds = [("bIsActive0", "bAnimationDriven"), ("bIsActive1", "RDF_IsDodging")] + bind_extra
    out.append(binding_set(isact_bs, binds))
    flags = "".join(p("bIsActive%d" % i, "false") + p("bInvertActive%d" % i, "false") for i in range(5))
    out.append(node(isact, "BSIsActiveModifier", "0xb0fde45a", "\n".join([
        p("variableBindingSet", isact_bs), p("userData", "2"), p("name", nm + "_IsActiveMod"), p("enable", "true"),
        flags])))
    # outer blender: dodge on everything except the arms, ArmLayer on the arms (complementary masks, weight 1 each)
    main_sel = dodge_selector(N, out, nm + "_Main")
    out.append(blender_child(outer_c0, main_sel, masks["not_arms"], 1.0, 1.0))
    out.append(blender_child(outer_c1, arm, masks["arms"], 1.0, 0.0))
    out.append(blender(outer, [outer_c0, outer_c1], BLEND_FLAGS_PLAIN))
    # ArmLayer: parametric lerp  RDF_ArmBlend 0 -> dodge copy, 1 -> stance
    arm_dodge = dodge_selector(N, out, nm + "_ArmDodge")
    out.append(binding_set(arm_bs, [("blendParameter", ARM_BLEND_VAR)]))
    out.append(blender_child(arm_p0, arm_dodge, "null", 0.0, 0.0))
    out.append(blender_child(arm_p1, stance, "null", 1.0, 0.0))
    out.append(blender(arm, [arm_p0, arm_p1], BLEND_FLAGS_PARAMETRIC, arm_bs))
    # StanceBlender: right selector on RightArm bones, left selector on LeftArm bones (char-property masks)
    r_sel = stance_selector(N, out, nm + "_StanceR", "R")
    l_sel = stance_selector(N, out, nm + "_StanceL", "L")
    st_c0_bs, st_c1_bs = N.new(), N.new()
    out.append(char_prop_binding_set(st_c0_bs, "boneWeights", CHAR_PROP["R"][1]))
    out.append(char_prop_binding_set(st_c1_bs, "boneWeights", CHAR_PROP["L"][1]))
    out.append(blender_child(st_c0, r_sel, "null", 1.0, 0.0, st_c0_bs))
    out.append(blender_child(st_c1, l_sel, "null", 1.0, 0.0, st_c1_bs))
    out.append(blender(stance, [st_c0, st_c1], BLEND_FLAGS_PLAIN))
    return st, out


# ----------------------------------------------------------------------------------------------- generation
def gen_master(existing_vars, fp=False):
    N = Names()
    V_ = V_FP if fp else V
    x = ["<patch>"]
    # events
    for e in EVENTS:
        x.append('  <append path="%s/eventNames"><hkcstring>%s</hkcstring></append>' % (V_["STRING"], e))
        x.append('  <append path="%s/eventInfos"><hkobject><hkparam name="flags">0</hkparam></hkobject></append>'
                 % V_["DATA"])
    # variables: three parallel lists
    for name, typ, init in VARIABLES:
        val = f2i(init) if typ == "VARIABLE_TYPE_REAL" else int(init)
        x.append('  <append path="%s/variableNames"><hkcstring>%s</hkcstring></append>' % (V_["STRING"], name))
        x.append('  <append path="%s/wordVariableValues"><hkobject><hkparam name="value">%d</hkparam></hkobject></append>'
                 % (V_["VALUES"], val))
        x.append('  <append path="%s/variableInfos"><hkobject><hkparam name="role"><hkobject>'
                 '<hkparam name="role">ROLE_DEFAULT</hkparam><hkparam name="flags">0</hkparam></hkobject></hkparam>'
                 '<hkparam name="type">%s</hkparam></hkobject></append>' % (V_["DATA"], typ))
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
    if fp:
        arms = set(FP_ARM_BONES)
        masks = {"arms": N.new(), "not_arms": N.new()}
        new_nodes.append(bone_weight_array(masks["arms"], [1.0 if b in arms else 0.0 for b in range(N_BONES_FP)]))
        new_nodes.append(bone_weight_array(masks["not_arms"], [0.0 if b in arms else 1.0 for b in range(N_BONES_FP)]))
        st_a, nodes_a = build_state(STATE_A[0], STATE_A[1], STATE_B[1], N, fx_nodes, bind_extra, masks, V_, True)
        st_b, nodes_b = build_state(STATE_B[0], STATE_B[1], STATE_A[1], N, fx_nodes, bind_extra, masks, V_, True)
    else:
        arms = set(RIGHT_ARM_BONES) | set(LEFT_ARM_BONES)
        masks = {"arms": N.new(), "not_arms": N.new()}
        new_nodes.append(bone_weight_array(masks["arms"], [1.0 if b in arms else 0.0 for b in range(N_BONES)]))
        new_nodes.append(bone_weight_array(masks["not_arms"], [0.0 if b in arms else 1.0 for b in range(N_BONES)]))
        st_a, nodes_a = build_state(STATE_A[0], STATE_A[1], STATE_B[1], N, fx_nodes, bind_extra, masks)
        st_b, nodes_b = build_state(STATE_B[0], STATE_B[1], STATE_A[1], N, fx_nodes, bind_extra, masks)
    new_nodes += nodes_a + nodes_b
    x.append('  <insert path="">')
    x += ["    " + n.replace("\n", "\n    ") for n in new_nodes]
    x.append("  </insert>")
    x.append('  <append path="%s/states">%s</append>' % (V_["ROOT_SM"], st_a))
    x.append('  <append path="%s/states">%s</append>' % (V_["ROOT_SM"], st_b))
    for event, (_, state_id) in START_EVENTS:
        x.append('  <append path="%s/transitions">%s</append>'
                 % (V_["ROOT_WILDCARD"], transition(fx_nodes["in"], event, state_id,
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
    for v in HAND_VARS.values():
        chk(v in g.var_index, "variable %s exists (stance selectors)" % v)
    for side, (cp, idx) in CHAR_PROP.items():
        chk(len(g.char_prop_names) > idx and g.char_prop_names[idx] == cp,
            "characterPropertyNames[%d] == %s (arm mask binding)" % (idx, cp))
    return ok


def verify_ids_fp(root):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import hkxcheck as H
    g = H.Graph(os.path.join(root, "_work", "fp_nem_xml", "0_master_nem.xml"))
    ok = True

    def chk(cond, msg):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + "[1P] " + msg)
        ok &= bool(cond)
    V1 = V_FP
    chk(g.objs[V1["STRING"]].cls == "hkbBehaviorGraphStringData", V1["STRING"] + " StringData")
    chk(g.objs[V1["VALUES"]].cls == "hkbVariableValueSet", V1["VALUES"] + " VariableValueSet")
    chk(g.objs[V1["DATA"]].cls == "hkbBehaviorGraphData", V1["DATA"] + " GraphData")
    chk(g.objs[V1["ROOT_SM"]].text("name") == "RootBehaviorGraph", V1["ROOT_SM"] + " RootBehaviorGraph")
    chk(g.objs[V1["ROOT_SM"]].text("wildcardTransitions") == V1["ROOT_WILDCARD"], V1["ROOT_WILDCARD"] + " wildcard array")
    chk(g.objs[V1["FX_STAGGER_PLAYER"]].text("name") == "FastBlendResetTransition", V1["FX_STAGGER_PLAYER"])
    for _f, _n, cid, expr in V1["RETURN_CONDS"]:
        chk(g.objs[cid].text("expression") == expr, cid + " " + expr)
    sm = g.objs[V1["ROOT_SM"]]
    sids = {g.objs[x].intval("stateId"): g.objs[x].text("name") for x in H.sm_states(g, sm)}
    chk(sids.get(DEFAULT_STATE_ID) == "Default_State", "stateId 14 Default_State")
    chk(sids.get(PLAYER_STAGGER_STATE_ID) == "PlayerStagger_State", "stateId 48 PlayerStagger_State")
    chk(STATE_A[1] not in sids and STATE_B[1] not in sids, "9701/9702 free")
    for v_ in list(EXISTING_VARS) + list(HAND_VARS.values()) + [FP_DRAWN_VAR]:
        chk(v_ in g.var_index, "variable %s exists" % v_)
    for e in ["IdleForceDefaultState", "StaggerPlayer"]:
        chk(e in g.ev_index, "event %s exists" % e)
    dsm = [x for x in H.state_machines(g) if x.text("name") == "Default_Behavior"][0]
    dids = sorted(g.objs[x].intval("stateId") for x in H.sm_states(g, dsm))
    chk(dids == sorted(FP_DRAWN), "Default_Behavior stateIds %s == FP_DRAWN keys" % dids)
    return ok


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--verify-ids" in sys.argv:
        return 0 if (verify_ids(root) & verify_ids_fp(root)) else 1
    out = args[0] if args else os.path.join(root, "behavior", "Pandora_Engine", "mod", CODE)
    # DF_IsDodging is NOT in this modlist's 0_master -> do not bind. Pass --with-df to force when it exists.
    existing = set(OPTIONAL_BIND_VARS) if "--with-df" in sys.argv else set()
    master, n_nodes, binds = gen_master(existing)
    write(os.path.join(out, "info.xml"),
          '<?xml version="1.0" encoding="utf-8"?>\n<mod code="%s">\n  <name>%s</name>\n  <author>%s</author>\n'
          '  <site>null</site>\n</mod>\n' % (CODE, MOD_NAME, AUTHOR))
    write(os.path.join(out, "patches", "0_master.xml"), master)
    fp_master, fp_nodes, _ = gen_master(set(), fp=True)
    write(os.path.join(out, "patches", FP_PATCH_NAME), fp_master)
    print("first person: %d new nodes -> patches/%s" % (fp_nodes, FP_PATCH_NAME))
    ch = gen_character()
    write(os.path.join(out, "patches", "defaultmale_character.xml"), ch)
    write(os.path.join(out, "patches", "defaultfemale_character.xml"), ch)
    print("wrote %s: %d new nodes, extra bindings %s" % (out, n_nodes, binds))
    return 0


if __name__ == "__main__":
    sys.exit(main())

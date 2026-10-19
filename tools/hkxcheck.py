#!/usr/bin/env python3
"""hkxcheck - sanity checker / inspector for Havok behavior XML (hkxc / serde-hkx or Nemesis hkxcmd output).

Works on the XML that `hkxc convert -v xml` (serde-hkx) or `hkxcmd convert -v:xml` produces for a
Skyrim behavior graph (hkbBehaviorGraph root).  Pure stdlib, Python 3.8+.

Usage
  hkxcheck.py FILE.xml                       run all checks (exit 1 if any ERROR)
  hkxcheck.py FILE.xml --summary NAME|#ID    print a named state machine (states, transitions, effects)
  hkxcheck.py FILE.xml --list-sm             list every state machine (id, name, #states)
  hkxcheck.py FILE.xml --find-event NAME     list where an event is used (NAME is exact, or glob with * ?)
  hkxcheck.py FILE.xml --find-var NAME       list where a variable is bound/used
  hkxcheck.py FILE.xml --event NAME          print the numeric event id(s) for NAME
  hkxcheck.py FILE.xml --object #ID          dump one object's params (short)
  hkxcheck.py FILE.xml --tree #ID [--depth N]  generator/modifier subtree: clips+triggers, expressions, bindings
  hkxcheck.py FILE.xml --new-from N          limit "new state machine" checks to SMs whose name matches regex N
                                             (default: check every state machine)
  hkxcheck.py FILE.xml --require-event N --require-var V   fail if a name is missing (verify a Pandora run)
  hkxcheck.py FILE.xml --character CHAR.xml  every clip animationName must be in the character's animationNames
  hkxcheck.py FILE.xml --strict              treat WARN as failure (exit 2)

Checks (ERROR unless noted)
  * every '#xxxx' pointer token resolves to an hkobject
  * eventNames/eventInfos lengths match; variableNames/variableInfos/wordVariableValues(+quad/variant) match;
    characterPropertyNames/characterPropertyInfos match
  * every event id used (transition eventId, state enter/exit notify events, clip triggers, SM event fields, ...)
    is -1 or  < len(eventNames)
  * every variableIndex used (variable bindings, syncVariableIndex, ...) is -1 or < len(variableNames)
    (character-property bindings are checked against characterPropertyNames)
  * per state machine: unique stateIds, startStateId exists, every transition's toStateId exists,
    transitions without effect object -> INFO (WARN when --new-from is used; vanilla has a few)
  * WARN: duplicate event/variable names, unreferenced hkobjects (informational, off by default: --orphans)
"""
import argparse
import fnmatch
import re
import sys
import xml.etree.ElementTree as ET

PTR_RE = re.compile(r"^#[^\s]+$")
EVENT_ID_PARAMS = {
    "eventId", "enterEventId", "exitEventId", "returnToPreviousStateEventId", "randomTransitionEventId",
    "transitionToNextHigherStateEventId", "transitionToNextLowerStateEventId", "enterNotifyEventId",
}
# hkparam 'id' children under an ancestor hkparam whose name matches this are event ids
EVENT_ANCESTOR_RE = re.compile(r"(?i)event|trigger")
EVENT_ANCESTOR_EXCLUDE = {"eventNames", "eventInfos"}
VARIABLE_INDEX_PARAMS_RE = re.compile(r"(?i)^(variableIndex|syncVariableIndex|.*VariableIndex)$")


class Obj:
    __slots__ = ("name", "cls", "el", "params")

    def __init__(self, el):
        self.el = el
        self.name = el.get("name")
        self.cls = el.get("class")
        self.params = {p.get("name"): p for p in el.findall("hkparam")}

    def text(self, p, default=None):
        e = self.params.get(p)
        if e is None:
            return default
        return (e.text or "").strip()

    def intval(self, p, default=None):
        t = self.text(p)
        try:
            return int(t)
        except (TypeError, ValueError):
            return default

    def label(self):
        return f"{self.name} {self.cls} '{self.text('name', '')}'"


class Graph:
    def __init__(self, path):
        self.path = path
        tree = ET.parse(path)
        root = tree.getroot()
        self.root = root
        self.toplevel = root.get("toplevelobject")
        self.objs = {}
        self.order = []
        for el in root.iter("hkobject"):
            if el.get("name"):
                o = Obj(el)
                self.objs[o.name] = o
                self.order.append(o)
        self.by_class = {}
        for o in self.order:
            self.by_class.setdefault(o.cls, []).append(o)
        self.event_names = []
        self.variable_names = []
        self.char_prop_names = []
        self.attribute_names = []
        self.animation_names = []
        self.n_event_infos = self.n_var_infos = self.n_word = self.n_quad = self.n_variant = None
        self.n_charprop_infos = None
        self._load_tables()

    # ---- tables -----------------------------------------------------------------------------------------
    @staticmethod
    def _strings(o, p):
        e = o.params.get(p)
        if e is None:
            return []
        return [(c.text or "") for c in e.findall("hkcstring")]

    @staticmethod
    def _count(o, p):
        e = o.params.get(p)
        if e is None:
            return None
        kids = len(e.findall("hkobject"))
        if kids:
            return kids  # count what is really there; numelements can lie in hand-edited files
        n = e.get("numelements")
        return int(n) if n is not None else 0

    def _load_tables(self):
        for o in self.by_class.get("hkbBehaviorGraphStringData", []):
            self.event_names = self._strings(o, "eventNames")
            self.variable_names = self._strings(o, "variableNames")
            self.attribute_names = self._strings(o, "attributeNames")
            self.char_prop_names = self._strings(o, "characterPropertyNames")
        for o in self.by_class.get("hkbCharacterStringData", []):
            self.animation_names = self._strings(o, "animationNames")
        for o in self.by_class.get("hkbBehaviorGraphData", []):
            self.n_event_infos = self._count(o, "eventInfos")
            self.n_var_infos = self._count(o, "variableInfos")
            self.n_charprop_infos = self._count(o, "characterPropertyInfos")
        for o in self.by_class.get("hkbVariableValueSet", []):
            self.n_word = self._count(o, "wordVariableValues")
            self.n_quad = self._count(o, "quadVariableValues")
            self.n_variant = self._count(o, "variantVariableValues")
        self.ev_index = {}
        for i, n in enumerate(self.event_names):
            self.ev_index.setdefault(n, []).append(i)
        self.var_index = {}
        for i, n in enumerate(self.variable_names):
            self.var_index.setdefault(n, []).append(i)

    def event_name(self, i):
        if i is None:
            return "?"
        if i == -1:
            return "-"
        if 0 <= i < len(self.event_names):
            return self.event_names[i]
        return f"<BAD EVENT {i}>"

    def var_name(self, i):
        if i == -1:
            return "-"
        if 0 <= i < len(self.variable_names):
            return self.variable_names[i]
        return f"<BAD VAR {i}>"

    def objname(self, ptr):
        o = self.objs.get(ptr)
        if not o:
            return f"{ptr}(MISSING)" if ptr and ptr != "null" else "null"
        n = o.text("name", "")
        return f"{ptr} {o.cls}{(' ' + repr(n)) if n else ''}"

    # ---- walking ----------------------------------------------------------------------------------------
    def walk_params(self, o):
        """yield (path, ancestors(list of hkparam names), param_element) for every hkparam under object o."""
        def rec(el, path, anc):
            for p in el.findall("hkparam"):
                nm = p.get("name")
                yield (path + [nm], anc, p)
                for k, ho in enumerate(p.findall("hkobject")):
                    yield from rec(ho, path + [f"{nm}[{k}]"], anc + [nm])
        yield from rec(o.el, [], [])


# ---------------------------------------------------------------------------------------------------------
def collect_refs(g, o):
    """every pointer token in object o: list of (path, token)."""
    out = []
    for path, anc, p in g.walk_params(o):
        t = (p.text or "").strip()
        if not t or p.find("hkobject") is not None or p.find("hkcstring") is not None:
            continue
        toks = t.split()
        if all(PTR_RE.match(x) for x in toks):
            for x in toks:
                out.append(("/".join(path), x))
    return out


def collect_event_ids(g, o):
    out = []
    for path, anc, p in g.walk_params(o):
        nm = p.get("name")
        t = (p.text or "").strip()
        if p.find("hkobject") is not None or not t:
            continue
        is_ev = False
        if nm in EVENT_ID_PARAMS:
            is_ev = True
        elif nm == "id" and anc and EVENT_ANCESTOR_RE.search(anc[-1]) and anc[-1] not in EVENT_ANCESTOR_EXCLUDE:
            is_ev = True
        if is_ev:
            try:
                out.append(("/".join(path), int(t)))
            except ValueError:
                out.append(("/".join(path), None))
    return out


def collect_var_indices(g, o):
    """list of (path, index, kind) kind in {'var','prop'}."""
    out = []
    if o.cls == "hkbVariableBindingSet":
        for path, anc, p in g.walk_params(o):
            pass
        b = o.params.get("bindings")
        if b is not None:
            for k, ho in enumerate(b.findall("hkobject")):
                prm = {x.get("name"): (x.text or "").strip() for x in ho.findall("hkparam")}
                try:
                    idx = int(prm.get("variableIndex", "-1"))
                except ValueError:
                    idx = None
                kind = "prop" if "CHARACTER_PROPERTY" in prm.get("bindingType", "") else "var"
                out.append((f"bindings[{k}]({prm.get('memberPath', '')})", idx, kind))
        return out
    for path, anc, p in g.walk_params(o):
        nm = p.get("name")
        if nm and VARIABLE_INDEX_PARAMS_RE.match(nm) and p.find("hkobject") is None:
            try:
                out.append(("/".join(path), int((p.text or "").strip()), "var"))
            except ValueError:
                pass
    return out


# ---------------------------------------------------------------------------------------------------------
class Report:
    def __init__(self):
        self.errors = []
        self.warns = []
        self.infos = []

    def err(self, m):
        self.errors.append(m)

    def warn(self, m):
        self.warns.append(m)

    def info(self, m):
        self.infos.append(m)


def state_machines(g):
    return g.by_class.get("hkbStateMachine", [])


def sm_states(g, sm):
    t = sm.text("states", "")
    return [x for x in t.split() if PTR_RE.match(x)]


def trans_array(g, ptr):
    """-> list of dict(transition fields) for an hkbStateMachineTransitionInfoArray pointer."""
    o = g.objs.get(ptr) if ptr and ptr != "null" else None
    if not o:
        return []
    out = []
    tp = o.params.get("transitions")
    if tp is None:
        return out
    for ho in tp.findall("hkobject"):
        d = {x.get("name"): (x.text or "").strip() for x in ho.findall("hkparam")}
        trig = ho.find("hkparam[@name='triggerInterval']")
        d["_ho"] = ho
        out.append(d)
    return out


def check(g, rep, sm_filter=None):
    # --- table lengths
    if len(g.event_names) != (g.n_event_infos if g.n_event_infos is not None else len(g.event_names)):
        rep.err(f"eventNames={len(g.event_names)} but eventInfos={g.n_event_infos}")
    if g.n_var_infos is not None and len(g.variable_names) != g.n_var_infos:
        rep.err(f"variableNames={len(g.variable_names)} but variableInfos={g.n_var_infos}")
    if g.n_word is not None and g.n_var_infos is not None:
        # quad/variant variables occupy a slot in variableNames but not in wordVariableValues
        if g.n_word > len(g.variable_names):
            rep.err(f"wordVariableValues={g.n_word} > variableNames={len(g.variable_names)}")
        elif g.n_word != len(g.variable_names):
            rep.info(f"wordVariableValues={g.n_word} variableNames={len(g.variable_names)} "
                     f"(ok if the rest are quad/variant vars: quad={g.n_quad} variant={g.n_variant})")
    if g.n_charprop_infos is not None and g.n_charprop_infos != len(g.char_prop_names):
        rep.err(f"characterPropertyNames={len(g.char_prop_names)} but characterPropertyInfos={g.n_charprop_infos}")
    for nm, idxs in g.ev_index.items():
        if len(idxs) > 1:
            rep.warn(f"duplicate event name '{nm}' at indices {idxs}")
    for nm, idxs in g.var_index.items():
        if len(idxs) > 1:
            rep.warn(f"duplicate variable name '{nm}' at indices {idxs}")
    if g.toplevel and g.toplevel not in g.objs:
        rep.err(f"toplevelobject {g.toplevel} does not exist")

    nE, nV, nP = len(g.event_names), len(g.variable_names), len(g.char_prop_names)
    for o in g.order:
        for path, tok in collect_refs(g, o):
            if tok not in g.objs:
                rep.err(f"{o.label()}: pointer {tok} at {path} does not resolve")
        for path, ev in collect_event_ids(g, o):
            if ev is None:
                rep.err(f"{o.label()}: non-numeric event id at {path}")
            elif ev < -1 or ev >= nE:
                rep.err(f"{o.label()}: event id {ev} at {path} out of range (eventNames={nE})")
        for path, idx, kind in collect_var_indices(g, o):
            if idx is None:
                rep.err(f"{o.label()}: non-numeric variable index at {path}")
                continue
            lim = nP if kind == "prop" else nV
            if idx < -1 or idx >= lim:
                rep.err(f"{o.label()}: {'characterProperty' if kind == 'prop' else 'variable'} index {idx} "
                        f"at {path} out of range ({lim})")

    # --- state machines
    pat = re.compile(sm_filter) if sm_filter else None
    for sm in state_machines(g):
        if pat and not pat.search(sm.text("name", "")):
            continue
        ids = {}
        for sp in sm_states(g, sm):
            so = g.objs.get(sp)
            if not so:
                continue  # reported as unresolved pointer already
            if so.cls != "hkbStateMachineStateInfo":
                rep.err(f"{sm.label()}: states entry {sp} is {so.cls}, expected hkbStateMachineStateInfo")
                continue
            sid = so.intval("stateId")
            if sid in ids:
                rep.err(f"{sm.label()}: duplicate stateId {sid} ({ids[sid].label()} and {so.label()})")
            ids[sid] = so
        start = sm.intval("startStateId")
        if start is not None and ids and start not in ids:
            rep.err(f"{sm.label()}: startStateId {start} is not a state of this machine")
        arrays = [("wildcard", sm.text("wildcardTransitions", "null"), None)]
        for sid, so in ids.items():
            arrays.append((f"state {sid} '{so.text('name', '')}'", so.text("transitions", "null"), so))
        for where, ptr, so in arrays:
            for d in trans_array(g, ptr):
                to = None
                try:
                    to = int(d.get("toStateId", ""))
                except ValueError:
                    rep.err(f"{sm.label()} {where}: bad toStateId {d.get('toStateId')!r}")
                if to is not None and to not in ids:
                    rep.err(f"{sm.label()} {where}: transition (event {g.event_name(int(d.get('eventId', '-1')))}) "
                            f"-> toStateId {to} does not exist")
                eff = d.get("transition", "null")
                flags = d.get("flags", "")
                if eff == "null" and "FLAG_IS_IMMEDIATE_BLEND" not in flags and "FLAG_IS_GLOBAL_WILDCARD" not in flags:
                    (rep.warn if sm_filter else rep.info)(f"{sm.label()} {where}: transition to {to} has no effect object and is not immediate")
                ev = d.get("eventId")
                if ev is not None:
                    try:
                        evi = int(ev)
                    except ValueError:
                        continue
                    if evi == -1:
                        rep.warn(f"{sm.label()} {where}: transition to {to} has eventId -1 (never fires by event)")


# ---------------------------------------------------------------------------------------------------------
def fmt_effect(g, ptr):
    if not ptr or ptr == "null":
        return "no-effect"
    o = g.objs.get(ptr)
    if not o:
        return f"{ptr}(MISSING)"
    if o.cls == "hkbBlendingTransitionEffect":
        return (f"{o.cls.replace('hkb', '')} '{o.text('name', '')}' dur={o.text('duration')} "
                f"toStartFrac={o.text('toGeneratorStartTimeFraction')} flags={o.text('flags')} "
                f"end={(o.text('endMode') or '').replace('END_MODE_', '')} "
                f"curve={(o.text('blendCurve') or '').replace('BLEND_CURVE_', '')} "
                f"selfTrans={(o.text('selfTransitionMode') or '').replace('SELF_TRANSITION_MODE_', '')}")
    if o.cls == "hkbGeneratorTransitionEffect":
        return f"GeneratorTransitionEffect '{o.text('name', '')}'"
    return f"{o.cls} '{o.text('name', '')}'"


def short_flags(f):
    f = (f or "").replace("FLAG_", "")
    return f


def event_payload_events(g, el):
    """events listed in an hkbStateMachineEventPropertyArray pointer (state enter/exit)."""
    return None


def notify_events(g, ptr):
    if not ptr or ptr == "null":
        return []
    o = g.objs.get(ptr)
    if not o:
        return [f"{ptr}(MISSING)"]
    out = []
    ep = o.params.get("events")
    if ep is None:
        return out
    for ho in ep.findall("hkobject"):
        for p in ho.findall("hkparam"):
            if p.get("name") == "id":
                try:
                    out.append(g.event_name(int(p.text.strip())) + f"({p.text.strip()})")
                except Exception:
                    out.append("?")
    return out


def generator_label(g, ptr):
    o = g.objs.get(ptr) if ptr and ptr != "null" else None
    if not o:
        return "null"
    s = f"{ptr} {o.cls.replace('hkb', '')} '{o.text('name', '')}'"
    if o.cls == "hkbBehaviorReferenceGenerator":
        s += f" -> {o.text('behaviorName')}"
    if o.cls == "hkbClipGenerator":
        s += f" anim={o.text('animationName')} mode={(o.text('mode') or '').replace('MODE_', '')}"
    return s


def summary(g, key):
    cands = []
    for sm in state_machines(g):
        if key == sm.name or sm.text("name", "") == key or (key.startswith("~") and re.search(key[1:], sm.text("name", ""))):
            cands.append(sm)
    if not cands:
        # substring fallback
        cands = [sm for sm in state_machines(g) if key.lower() in sm.text("name", "").lower()]
    if not cands:
        print(f"no state machine matching {key!r}; use --list-sm", file=sys.stderr)
        return 1
    for sm in cands:
        print("=" * 100)
        print(f"{sm.name} hkbStateMachine '{sm.text('name')}'  startStateId={sm.text('startStateId')} "
              f"selfTransitionMode={(sm.text('selfTransitionMode') or '').replace('SELF_TRANSITION_MODE_', '')} "
              f"returnToPrev={g.event_name(sm.intval('returnToPreviousStateEventId', -1))}")
        ids = {}
        for sp in sm_states(g, sm):
            so = g.objs.get(sp)
            if so:
                ids[so.intval("stateId")] = so
        wild = trans_array(g, sm.text("wildcardTransitions", "null"))
        print(f"  wildcard transitions ({len(wild)}):")
        for d in wild:
            print_transition(g, d, ids, "    ")
        print(f"  states ({len(ids)}):")
        for sid in sorted(ids, key=lambda x: (x is None, x)):
            so = ids[sid]
            print(f"  [{sid}] {so.name} '{so.text('name', '')}'  gen={generator_label(g, so.text('generator', 'null'))}")
            en = notify_events(g, so.text("enterNotifyEvents", "null"))
            ex = notify_events(g, so.text("exitNotifyEvents", "null"))
            if en:
                print(f"       enter: {', '.join(en)}")
            if ex:
                print(f"       exit : {', '.join(ex)}")
            for d in trans_array(g, so.text("transitions", "null")):
                print_transition(g, d, ids, "       ")
    return 0


def print_transition(g, d, ids, indent):
    try:
        ev = int(d.get("eventId", "-1"))
    except ValueError:
        ev = None
    try:
        to = int(d.get("toStateId", "-1"))
    except ValueError:
        to = None
    tn = ids[to].text("name", "") if to in ids else "<MISSING>"
    print(f"{indent}{g.event_name(ev)}({ev}) -> [{to}] {tn}  prio={d.get('priority')} flags={short_flags(d.get('flags'))}"
          f"{' cond=' + d['condition'] if d.get('condition', 'null') != 'null' else ''}")
    print(f"{indent}   effect: {fmt_effect(g, d.get('transition'))}")


def find_event(g, pat):
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = _find_event(g, pat)
    seen = {}
    order = []
    for line in buf.getvalue().splitlines():
        if line not in seen:
            seen[line] = 0
            order.append(line)
        seen[line] += 1
    for line in order:
        print(line + (f"   (x{seen[line]})" if seen[line] > 1 and line.startswith("  ") else ""))
    return rc


def _find_event(g, pat):
    names = [n for n in g.event_names if fnmatch.fnmatchcase(n, pat) or n.lower() == pat.lower()]
    if not names:
        names = [n for n in g.event_names if fnmatch.fnmatchcase(n.lower(), pat.lower())]
    if not names:
        print(f"event {pat!r} not found in eventNames ({len(g.event_names)})")
        return 1
    for n in names:
        idxs = g.ev_index[n]
        print(f"event '{n}' index {idxs} flags={event_flags(g, idxs[0])}")
        ids = set(idxs)
        for o in g.order:
            hits = [(p, ev) for p, ev in collect_event_ids(g, o) if ev in ids]
            if not hits:
                continue
            # context for transitions
            if o.cls == "hkbStateMachineTransitionInfoArray":
                owners = [(sm, "wildcard") for sm in state_machines(g) if sm.text("wildcardTransitions") == o.name]
                owners += [(so, "state") for so in g.by_class.get("hkbStateMachineStateInfo", [])
                           if so.text("transitions") == o.name]
                desc = ""
                for ow, kind in owners:
                    if kind == "wildcard":
                        desc = f"wildcard of SM {ow.name} '{ow.text('name')}'"
                    else:
                        sm = next((s for s in state_machines(g) if ow.name in sm_states(g, s)), None)
                        desc = (f"state {ow.name} '{ow.text('name')}' (stateId {ow.text('stateId')}) of SM "
                                f"{sm.name if sm else '?'} '{sm.text('name') if sm else '?'}'")
                for d in trans_array(g, o.name):
                    try:
                        if int(d.get("eventId", "-1")) in ids:
                            print(f"  {o.name} transitions: {desc}: -> toStateId {d.get('toStateId')} "
                                  f"effect={fmt_effect(g, d.get('transition'))}")
                    except ValueError:
                        pass
            else:
                for p, ev in hits:
                    print(f"  {o.label()} :: {p} = {ev}")
    return 0


def event_flags(g, i):
    for o in g.by_class.get("hkbBehaviorGraphData", []):
        ep = o.params.get("eventInfos")
        if ep is None:
            return "?"
        items = ep.findall("hkobject")
        if i < len(items):
            for x in items[i].findall("hkparam"):
                if x.get("name") == "flags":
                    return (x.text or "").strip()
    return "?"


def find_var(g, pat):
    names = [n for n in g.variable_names if fnmatch.fnmatchcase(n.lower(), pat.lower())]
    if not names:
        print(f"variable {pat!r} not found ({len(g.variable_names)} variables)")
        return 1
    for n in names:
        idxs = g.var_index[n]
        print(f"variable '{n}' index {idxs}")
        ids = set(idxs)
        for o in g.order:
            for p, idx, kind in collect_var_indices(g, o):
                if kind == "var" and idx in ids:
                    owner = owner_of_binding(g, o)
                    print(f"  {o.label()} :: {p}{('   (bound by ' + owner + ')') if owner else ''}")
    return 0


def owner_of_binding(g, bs):
    if bs.cls != "hkbVariableBindingSet":
        return ""
    for o in g.order:
        if o.text("variableBindingSet") == bs.name:
            return f"{o.name} {o.cls} '{o.text('name', '')}'"
    return ""


def dump_object(g, ptr):
    o = g.objs.get(ptr)
    if not o:
        print("no such object")
        return 1
    print(o.label())
    for path, anc, p in g.walk_params(o):
        if p.find("hkobject") is not None or p.find("hkcstring") is not None:
            print(f"  {'/'.join(path)}: ({p.get('numelements', 'struct')})")
        else:
            t = " ".join((p.text or "").split())
            print(f"  {'/'.join(path)} = {t[:200]}")
    return 0


def tree(g, ptr, depth=6, ind=0, seen=None, out=None):
    """print the generator/modifier subtree below ptr (clips with triggers, expressions, variable bindings)."""
    seen = set() if seen is None else seen
    o = g.objs.get(ptr)
    if not o:
        print("  " * ind + f"{ptr} MISSING")
        return
    line = f"{ptr} {o.cls.replace('hkb', '')} '{o.text('name', '')}'"
    if o.cls == "hkbClipGenerator":
        line += f" anim={o.text('animationName')} mode={(o.text('mode') or '').replace('MODE_', '')} speed={o.text('playbackSpeed')}"
    if o.cls == "hkbEvaluateExpressionModifier":
        ex = g.objs.get(o.text("expressions", ""))
        if ex is not None:
            e = ex.params.get("expressionsData")
            exprs = [h.find("hkparam[@name='expression']").text for h in e.findall("hkobject")] if e is not None else []
            line += " expr=" + " ; ".join(exprs)
    bs = g.objs.get(o.text("variableBindingSet", "null"))
    if bs is not None:
        b = []
        for k, ho in enumerate(bs.params["bindings"].findall("hkobject")):
            prm = {x.get("name"): (x.text or "").strip() for x in ho.findall("hkparam")}
            vi = int(prm.get("variableIndex", "-1"))
            nm = g.var_name(vi) if "CHARACTER_PROPERTY" not in prm.get("bindingType", "") else (
                g.char_prop_names[vi] if 0 <= vi < len(g.char_prop_names) else "?")
            b.append(f"{prm.get('memberPath')}<-{nm}")
        line += "  bind[" + ", ".join(b) + "]"
    print("  " * ind + line)
    if o.cls == "hkbClipGenerator":
        tr = g.objs.get(o.text("triggers", "null"))
        if tr is not None:
            for t in tr.params["triggers"].findall("hkobject"):
                d = {x.get("name"): x for x in t.findall("hkparam")}
                idp = d["event"].find("hkobject/hkparam[@name='id']")
                eid = int(idp.text)
                print("  " * (ind + 2) + f"trigger t={d['localTime'].text} evt={g.event_name(eid)}({eid}) "
                      f"relEnd={d['relativeToEndOfClip'].text} annot={d['isAnnotation'].text}")
    if ptr in seen or ind >= depth:
        return
    seen.add(ptr)
    for path, anc, p in g.walk_params(o):
        nm = p.get("name")
        if nm in ("variableBindingSet", "triggers", "expressions", "transitions", "wildcardTransitions"):
            continue
        if p.find("hkobject") is not None:
            continue
        toks = (p.text or "").split()
        if toks and all(PTR_RE.match(x) for x in toks):
            for x in toks:
                if x in g.objs and g.objs[x].cls != "hkbStateMachineEventPropertyArray":
                    if g.objs[x].cls.startswith(("hkbStateMachineStateInfo",)):
                        tree(g, g.objs[x].text("generator", "null"), depth, ind + 1, seen)
                    else:
                        tree(g, x, depth, ind + 1, seen)


def list_sms(g):
    for sm in state_machines(g):
        print(f"{sm.name}  '{sm.text('name')}'  states={len(sm_states(g, sm))} start={sm.text('startStateId')} "
              f"wildcard={sm.text('wildcardTransitions')}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Havok behavior XML checker")
    ap.add_argument("xml")
    ap.add_argument("--summary", metavar="NAME|#ID")
    ap.add_argument("--list-sm", action="store_true")
    ap.add_argument("--find-event", metavar="NAME")
    ap.add_argument("--find-var", metavar="NAME")
    ap.add_argument("--event", metavar="NAME")
    ap.add_argument("--object", metavar="#ID")
    ap.add_argument("--tree", metavar="#ID", help="print generator/modifier subtree (clips, triggers, expressions, bindings)")
    ap.add_argument("--depth", type=int, default=6, help="--tree depth")
    ap.add_argument("--new-from", metavar="REGEX", help="only run state-machine checks on SMs whose name matches")
    ap.add_argument("--require-event", metavar="NAME", action="append", default=[],
                    help="ERROR if this event name is not in eventNames (repeatable; for post-Pandora verification)")
    ap.add_argument("--require-var", metavar="NAME", action="append", default=[],
                    help="ERROR if this variable name is not in variableNames (repeatable)")
    ap.add_argument("--character", metavar="CHAR.xml",
                    help="also check every hkbClipGenerator.animationName exists in this character file's animationNames")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--max", type=int, default=40, help="max messages printed per severity")
    a = ap.parse_args(argv)
    g = Graph(a.xml)
    if a.list_sm:
        return list_sms(g)
    if a.summary:
        return summary(g, a.summary)
    if a.find_event:
        return find_event(g, a.find_event)
    if a.find_var:
        return find_var(g, a.find_var)
    if a.event:
        for n in g.event_names:
            if fnmatch.fnmatchcase(n.lower(), a.event.lower()):
                print(f"{g.ev_index[n]} {n}")
        return 0
    if a.object:
        return dump_object(g, a.object)
    if a.tree:
        tree(g, a.tree, a.depth)
        return 0
    rep = Report()
    check(g, rep, a.new_from)
    if a.character:
        ch = Graph(a.character)
        have = {x.lower().replace("/", "\\") for x in ch.animation_names}
        missing = {}
        for o in g.by_class.get("hkbClipGenerator", []):
            an = (o.text("animationName", "") or "").lower().replace("/", "\\")
            if an and an not in have:
                missing.setdefault(an, []).append(o.name)
        for an, users in sorted(missing.items()):
            rep.err(f"clip animation '{an}' (used by {', '.join(users[:3])}) is not in {a.character} animationNames")
        rep.info(f"checked {len(g.by_class.get('hkbClipGenerator', []))} clips against {len(ch.animation_names)} character animations")
    for n in a.require_event:
        if n not in g.ev_index:
            rep.err(f"required event '{n}' is not in eventNames")
    for n in a.require_var:
        if n not in g.var_index:
            rep.err(f"required variable '{n}' is not in variableNames")
    print(f"{a.xml}: {len(g.objs)} objects, {len(g.event_names)} events, {len(g.variable_names)} variables, "
          f"{len(g.char_prop_names)} character properties, {len(state_machines(g))} state machines, "
          f"{len(g.animation_names)} animationNames")
    for label, lst in (("ERROR", rep.errors), ("WARN", rep.warns), ("INFO", rep.infos)):
        for m in lst[: a.max]:
            print(f"{label}: {m}")
        if len(lst) > a.max:
            print(f"{label}: ... and {len(lst) - a.max} more")
    print(f"result: {len(rep.errors)} error(s), {len(rep.warns)} warning(s)")
    if rep.errors:
        return 1
    if a.strict and rep.warns:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())

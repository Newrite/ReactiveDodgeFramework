#!/usr/bin/env python3
"""Self-test for hkxcheck.py: builds a tiny valid behavior XML, then mutates it and expects specific errors.

    python tools/test_hkxcheck.py          (no dependencies, no game files needed)
"""
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hkxcheck as H  # noqa: E402

TEMPLATE = """<?xml version="1.0" encoding="ascii"?>
<hkpackfile classversion="8" contentsversion="hk_2010.2.0-r1" toplevelobject="#0001">
<hksection name="__data__">
<hkobject name="#0001" class="hkRootLevelContainer"><hkparam name="namedVariants" numelements="1"><hkobject>
  <hkparam name="name">hkbBehaviorGraph</hkparam><hkparam name="className">hkbBehaviorGraph</hkparam><hkparam name="variant">#0002</hkparam>
</hkobject></hkparam></hkobject>
<hkobject name="#0002" class="hkbBehaviorGraph"><hkparam name="name">g</hkparam><hkparam name="rootGenerator">#0010</hkparam><hkparam name="data">#0003</hkparam></hkobject>
<hkobject name="#0003" class="hkbBehaviorGraphData">
  <hkparam name="variableInfos" numelements="2">__VARINFOS__</hkparam>
  <hkparam name="characterPropertyInfos" numelements="0"></hkparam>
  <hkparam name="eventInfos" numelements="__NEVINFOS__">__EVINFOS__</hkparam>
  <hkparam name="variableInitialValues">#0005</hkparam><hkparam name="stringData">#0004</hkparam></hkobject>
<hkobject name="#0004" class="hkbBehaviorGraphStringData">
  <hkparam name="eventNames" numelements="3"><hkcstring>evA</hkcstring><hkcstring>evB</hkcstring><hkcstring>evC</hkcstring></hkparam>
  <hkparam name="attributeNames" numelements="0"></hkparam>
  <hkparam name="variableNames" numelements="2"><hkcstring>varA</hkcstring><hkcstring>varB</hkcstring></hkparam>
  <hkparam name="characterPropertyNames" numelements="0"></hkparam></hkobject>
<hkobject name="#0005" class="hkbVariableValueSet"><hkparam name="wordVariableValues" numelements="2">
  <hkobject><hkparam name="value">0</hkparam></hkobject><hkobject><hkparam name="value">0</hkparam></hkobject></hkparam>
  <hkparam name="quadVariableValues" numelements="0"></hkparam><hkparam name="variantVariableValues" numelements="0"></hkparam></hkobject>
<hkobject name="#0006" class="hkbBlendingTransitionEffect"><hkparam name="name">blend</hkparam><hkparam name="duration">0.200000</hkparam></hkobject>
<hkobject name="#0007" class="hkbStateMachineTransitionInfoArray"><hkparam name="transitions" numelements="1"><hkobject>
  <hkparam name="triggerInterval"><hkobject><hkparam name="enterEventId">-1</hkparam><hkparam name="exitEventId">-1</hkparam></hkobject></hkparam>
  <hkparam name="transition">#0006</hkparam><hkparam name="eventId">__TRANSEVENT__</hkparam><hkparam name="toStateId">__TOSTATE__</hkparam>
  <hkparam name="flags">0</hkparam></hkobject></hkparam></hkobject>
<hkobject name="#0008" class="hkbStateMachineStateInfo"><hkparam name="name">S0</hkparam><hkparam name="stateId">0</hkparam><hkparam name="transitions">#0007</hkparam><hkparam name="generator">#0011</hkparam></hkobject>
<hkobject name="#0009" class="hkbStateMachineStateInfo"><hkparam name="name">S1</hkparam><hkparam name="stateId">__SID1__</hkparam><hkparam name="transitions">null</hkparam><hkparam name="generator">#0011</hkparam></hkobject>
<hkobject name="#0010" class="hkbStateMachine"><hkparam name="name">SM</hkparam><hkparam name="startStateId">0</hkparam>
  <hkparam name="states" numelements="2">#0008 #0009</hkparam><hkparam name="wildcardTransitions">null</hkparam></hkobject>
<hkobject name="#0011" class="hkbClipGenerator"><hkparam name="name">clip</hkparam><hkparam name="variableBindingSet">__BINDSET__</hkparam><hkparam name="animationName">a.hkx</hkparam><hkparam name="triggers">null</hkparam></hkobject>
<hkobject name="#0012" class="hkbVariableBindingSet"><hkparam name="bindings" numelements="1"><hkobject>
  <hkparam name="memberPath">playbackSpeed</hkparam><hkparam name="variableIndex">__VARIDX__</hkparam><hkparam name="bindingType">BINDING_TYPE_VARIABLE</hkparam></hkobject></hkparam></hkobject>
</hksection></hkpackfile>
"""

EV = "<hkobject><hkparam name=\"flags\">0</hkparam></hkobject>"
VI = "<hkobject><hkparam name=\"role\"><hkobject><hkparam name=\"role\">ROLE_DEFAULT</hkparam><hkparam name=\"flags\">0</hkparam></hkobject></hkparam><hkparam name=\"type\">VARIABLE_TYPE_REAL</hkparam></hkobject>"


def build(**kw):
    d = dict(__VARINFOS__=VI * 2, __NEVINFOS__="3", __EVINFOS__=EV * 3, __TRANSEVENT__="1", __TOSTATE__="1",
             __SID1__="1", __BINDSET__="#0012", __VARIDX__="1")
    d.update(kw)
    s = TEMPLATE
    for k, v in d.items():
        s = s.replace(k, v)
    return s


def run(xml, *args):
    with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False, encoding="utf-8") as f:
        f.write(xml)
        path = f.name
    buf = io.StringIO()
    try:
        with redirect_stdout(buf):
            rc = H.main([path, *args])
    finally:
        os.unlink(path)
    return rc, buf.getvalue()


class T(unittest.TestCase):
    def test_valid(self):
        rc, out = run(build())
        self.assertEqual(rc, 0, out)
        self.assertIn("0 error(s)", out)

    def test_event_out_of_range(self):
        rc, out = run(build(__TRANSEVENT__="3"))
        self.assertEqual(rc, 1)
        self.assertIn("event id 3", out)

    def test_eventinfos_mismatch(self):
        rc, out = run(build(__NEVINFOS__="2", __EVINFOS__=EV * 2))
        self.assertEqual(rc, 1)
        self.assertIn("eventNames=3 but eventInfos=2", out)

    def test_missing_state(self):
        rc, out = run(build(__TOSTATE__="7"))
        self.assertEqual(rc, 1)
        self.assertIn("toStateId 7 does not exist", out)

    def test_duplicate_state_id(self):
        rc, out = run(build(__SID1__="0"))
        self.assertEqual(rc, 1)
        self.assertIn("duplicate stateId 0", out)

    def test_dangling_pointer(self):
        rc, out = run(build(__BINDSET__="#0099"))
        self.assertEqual(rc, 1)
        self.assertIn("#0099 at variableBindingSet does not resolve", out)

    def test_variable_index(self):
        rc, out = run(build(__VARIDX__="2"))
        self.assertEqual(rc, 1)
        self.assertIn("variable index 2", out)

    def test_variableinfos_mismatch(self):
        rc, out = run(build(__VARINFOS__=VI))
        self.assertEqual(rc, 1)
        self.assertIn("variableNames=2 but variableInfos=1", out)

    def test_summary_and_find(self):
        rc, out = run(build(), "--summary", "SM")
        self.assertEqual(rc, 0)
        self.assertIn("evB(1) -> [1] S1", out)
        self.assertIn("dur=0.200000", out)
        rc, out = run(build(), "--find-event", "evB")
        self.assertEqual(rc, 0)
        self.assertIn("state #0008 'S0'", out)
        rc, out = run(build(), "--find-event", "nope")
        self.assertEqual(rc, 1)

    def test_require_event(self):
        rc, out = run(build(), "--require-event", "evA", "--require-event", "missingEv")
        self.assertEqual(rc, 1)
        self.assertIn("required event 'missingEv' is not in eventNames", out)


if __name__ == "__main__":
    unittest.main()

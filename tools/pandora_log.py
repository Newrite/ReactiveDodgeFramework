#!/usr/bin/env python3
"""Summarise Pandora's Engine.log: which edits FAILED, which were SKIPPED, grouped by mod.

  python tools/pandora_log.py                       # default log = <MO2>/mods/Pandora/Engine.log
  python tools/pandora_log.py --mod "Reactive"      # only lines mentioning this mod name / code fragment
  python tools/pandora_log.py --log other.log --all # also show INFO lines of interest

Patterns (all from Pandora source, see docs/research/behavior-pipeline.md section 7):
  Dispatcher > Mod "X" > PackFile "proj~file" > Apply > FAILED > <Type> <Element|Text> at <path>   edit not applied
  Pandora Parser > Mod "X" > Node "#id" > Pop > FAILED > Not found in pack file                       node id wrong
  Pandora Parser > Mod "X" > Path "p" > Parse > FAILED > Invalid format (missing '/')                 bad path attr
  Pandora Parser > Mod "X" > Node "#new" > Track > FAILED > Already exists in pack file               name collision
  Pandora Assembler > Mod "X" > File "f" > Load > FAILED                                              patch XML not well-formed
  Validator > proj~file > Duplicate Event|Variable > N > Index > i > SKIPPED                          dedup (usually fine)
  Validator > Nemesis Event ID > N > Index > NOT FOUND                                                (legacy path)
  Export > proj~file > FAILED                                                                         hkx not written (XML written instead!)
  Packfile > Active Nodes > Push > FAILED                                                             node deserialisation error: edit lost
Silent failures (NOT logged): unknown $eventID[..]$ / $variableID[..]$ -> -1; unknown '#name' pointer -> null.
Use tools/hkxcheck.py on the dumped output to catch those.
"""
import argparse
import collections
import re
import sys

DEFAULT = r"F:\STB\Skyrim\[STB] Mod Organizer\mods\Pandora\Engine.log"
PAT = re.compile(r"FAILED|SKIPPED|NOT FOUND|ERROR|FATAL|Already exists", re.I)
MOD = re.compile(r'Mod "([^"]+)"')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=DEFAULT)
    ap.add_argument("--mod", help="substring (case-insens.) of the mod name to filter on")
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    with open(a.log, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    by_mod = collections.defaultdict(list)
    other = []
    for ln in lines:
        if a.mod and a.mod.lower() not in ln.lower():
            continue
        m = MOD.search(ln)
        if PAT.search(ln) and not ln.startswith("WARN : FNIS Parser"):
            (by_mod[m.group(1)] if m else other).append(ln)
    n = 0
    for mod, ls in sorted(by_mod.items()):
        print(f"== {mod}: {len(ls)} problem line(s)")
        for ln in ls[:60]:
            print("  " + ln)
        n += len(ls)
    if other:
        print(f"== no mod attribution: {len(other)}")
        for ln in other[:60]:
            print("  " + ln)
        n += len(other)
    fnis = [ln for ln in lines if ln.startswith("WARN : FNIS Parser")]
    print(f"({len(fnis)} 'FNIS Parser ... FAILED' lines hidden: they only mean 'no FNIS list/graph in that folder')")
    if a.all:
        for ln in lines:
            if ln.startswith(("INFO : Pandora Mod", "INFO : Skyrim Patcher", "INFO : FNIS")):
                print(ln)
    print(f"total problem lines: {n}")
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())

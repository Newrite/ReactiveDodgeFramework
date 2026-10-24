#!/usr/bin/env python3
r"""Packs Reactive Dodge Framework as a FOMOD archive (MO2 / Vortex installer with choices).

    python tools/build_fomod.py [--no-copy] [--no-greatsword]

Inputs (build and generate them first: xmake build, tools/gen_rdf_patch.py, tools/gen_rdf_nemesis.py):
  build/windows/x64/releasedbg/ReactiveDodgeFramework.dll/.pdb, dist/ReactiveDodgeFramework.ini (defaults),
  dist/Interface/Translations, dist/Scripts (ReactiveDodge.pex + source), dist/CastOn (example rules + schema),
  docs/ru (the designer / animator guides, regenerate Settings.md with tools/gen_settings_reference.py),
  behavior/Pandora_Engine/mod/rdf,
  behavior/Nemesis_Engine/mod/rdfm,
  the dodge clips and the greatsword set from the MO2 mods (local fixtures, not in git).
Output: _work/fomod/ReactiveDodgeFramework-<version>.zip, also copied into MO2's downloads folder.

The clips are development placeholders built from The Ultimate Dodge Mod (base set) and a TK Dodge set
(greatsword): fine to hand to a friend, not to publish.
"""
import argparse
import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS = r"F:\STB\Skyrim\[STB] Mod Organizer\mods"
DOWNLOADS = r"F:\downloads"
STAGE = os.path.join(ROOT, "_work", "fomod", "stage")
NAME = "Reactive Dodge Framework"


def version():
    m = re.search(r'set_version\("([^"]+)"\)', open(os.path.join(ROOT, "xmake.lua"), encoding="utf-8").read())
    return m.group(1) if m else "0.0.0"


def copy(src, dst):
    if not os.path.exists(src):
        sys.exit("missing input: %s" % src)
    if os.path.isdir(src):
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)


def preset_ini(base_text, preset_path):
    """The default ini with a preset's keys set (the plugin reads one ini, so a preset is a full ini)."""
    text, section = base_text, None
    for line in open(preset_path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("["):
            section = line[1:-1]
            continue
        key, value = (x.strip() for x in line.split("=", 1))
        sec = re.search(r"^\[%s\]\s*$(.*?)(?=^\[|\Z)" % re.escape(section), text, re.M | re.S)
        if not sec:
            sys.exit("preset section [%s] not in the ini" % section)
        body, n = re.subn(r"^%s=.*$" % re.escape(key), "%s=%s" % (key, value), sec.group(1), count=1, flags=re.M)
        if not n:
            sys.exit("preset key %s not in [%s]" % (key, section))
        text = text[:sec.start(1)] + body + text[sec.end(1):]
    return text


INFO = """<?xml version="1.0" encoding="UTF-8"?>
<fomod>
\t<Name>{name}</Name>
\t<Author>newrite</Author>
\t<Version>{version}</Version>
\t<Description>Buffered, steerable, animation-driven dodge for Skyrim SE 1.5.97: chains, cancel windows into attack and block, i-frames, lock-on and first person, in-game settings through FLICK.</Description>
\t<Groups><element>Gameplay</element></Groups>
</fomod>
"""

CONFIG = """<?xml version="1.0" encoding="UTF-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
\t<moduleName>{name}</moduleName>
\t<requiredInstallFiles>
\t\t<folder source="00 Core" destination="" />
\t</requiredInstallFiles>
\t<installSteps order="Explicit">
\t\t<installStep name="Behavior engine">
\t\t\t<optionalFileGroups order="Explicit">
\t\t\t\t<group name="Which behavior engine do you run?" type="SelectExactlyOne">
\t\t\t\t\t<plugins order="Explicit">
\t\t\t\t\t\t<plugin name="Pandora Behaviour Engine+">
\t\t\t\t\t\t\t<description>Installs the Pandora patch (Pandora_Engine\\mod\\rdf). After installing, run Pandora, tick "Reactive Dodge Framework" and generate.</description>
\t\t\t\t\t\t\t<files><folder source="10 Pandora" destination="" /></files>
\t\t\t\t\t\t\t<typeDescriptor><type name="Recommended" /></typeDescriptor>
\t\t\t\t\t\t</plugin>
\t\t\t\t\t\t<plugin name="Nemesis Unlimited Behavior Engine">
\t\t\t\t\t\t\t<description>Installs the Nemesis patch (Nemesis_Engine\\mod\\rdfm). After installing, run Nemesis, tick "Reactive Dodge Framework", update the engine and launch it.</description>
\t\t\t\t\t\t\t<files><folder source="11 Nemesis" destination="" /></files>
\t\t\t\t\t\t\t<typeDescriptor><type name="Optional" /></typeDescriptor>
\t\t\t\t\t\t</plugin>
\t\t\t\t\t</plugins>
\t\t\t\t</group>
\t\t\t</optionalFileGroups>
\t\t</installStep>
\t\t<installStep name="Options">
\t\t\t<optionalFileGroups order="Explicit">
\t\t\t\t<group name="Steering" type="SelectExactlyOne">
\t\t\t\t\t<plugins order="Explicit">
\t\t\t\t\t\t<plugin name="Default">
\t\t\t\t\t\t\t<description>The move input can still steer a dodge after it starts, and the camera can turn a camera-facing dodge.</description>
\t\t\t\t\t\t\t<conditionFlags><flag name="steering">default</flag></conditionFlags>
\t\t\t\t\t\t\t<typeDescriptor><type name="Recommended" /></typeDescriptor>
\t\t\t\t\t\t</plugin>
\t\t\t\t\t\t<plugin name="Committed Dodge">
\t\t\t\t\t\t\t<description>Once a dodge has started, neither the move input nor the camera changes it (fMaxDeviation=0, bInputFromStartCamera=1, bHoldFacing=1). Everything can be changed later in the FLICK menu (F7) or the ini.</description>
\t\t\t\t\t\t\t<files><folder source="30 Preset Committed" destination="" priority="10" /></files>
\t\t\t\t\t\t\t<typeDescriptor><type name="Optional" /></typeDescriptor>
\t\t\t\t\t\t</plugin>
\t\t\t\t\t</plugins>
\t\t\t\t</group>
{sets}\t\t\t</optionalFileGroups>
\t\t</installStep>
\t</installSteps>
</config>
"""


SETS_GROUP = """\t\t\t\t<group name="Animation sets" type="SelectAny">
\t\t\t\t\t<plugins order="Explicit">
\t\t\t\t\t\t<plugin name="Greatsword dodge set (OAR)">
\t\t\t\t\t\t\t<description>Eight directional greatsword dodges for two-handed swords (not spears), through Open Animation Replacer.</description>
\t\t\t\t\t\t\t<files><folder source="20 Greatsword Set" destination="" /></files>
\t\t\t\t\t\t\t<typeDescriptor><type name="Optional" /></typeDescriptor>
\t\t\t\t\t\t</plugin>
\t\t\t\t\t</plugins>
\t\t\t\t</group>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-copy", action="store_true", help="do not copy the archive into MO2's downloads folder")
    ap.add_argument("--no-greatsword", action="store_true", help="leave the greatsword dodge set (and its installer option) out")
    args = ap.parse_args()
    ver = version()
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(STAGE)

    core = os.path.join(STAGE, "00 Core")
    build = os.path.join(ROOT, "build", "windows", "x64", "releasedbg")
    copy(os.path.join(build, "ReactiveDodgeFramework.dll"), os.path.join(core, "SKSE", "Plugins", "ReactiveDodgeFramework.dll"))
    copy(os.path.join(build, "ReactiveDodgeFramework.pdb"), os.path.join(core, "SKSE", "Plugins", "ReactiveDodgeFramework.pdb"))
    base_ini = os.path.join(ROOT, "dist", "ReactiveDodgeFramework.ini")
    copy(base_ini, os.path.join(core, "SKSE", "Plugins", "ReactiveDodgeFramework.ini"))
    copy(os.path.join(ROOT, "dist", "Interface"), os.path.join(core, "Interface"))
    copy(os.path.join(ROOT, "dist", "Scripts"), os.path.join(core, "Scripts"))
    # CastOn: the example rules (all disabled) and the JSON schema editors use
    copy(os.path.join(ROOT, "dist", "CastOn"), os.path.join(core, "SKSE", "Plugins", "ReactiveDodgeFramework", "CastOn"))
    # the designer / animator documentation (Russian for now), beside the plugin's own folder
    for name in sorted(os.listdir(os.path.join(ROOT, "docs", "ru"))):
        if name.endswith(".md"):
            copy(os.path.join(ROOT, "docs", "ru", name), os.path.join(core, "SKSE", "Plugins", "ReactiveDodgeFramework", "Docs", "ru", name))
    clips = os.path.join("meshes", "actors", "character", "animations", "RDF")
    copy(os.path.join(MODS, "AI - Reactive Dodge Framework", clips), os.path.join(core, clips))

    copy(os.path.join(ROOT, "behavior", "Pandora_Engine", "mod", "rdf"), os.path.join(STAGE, "10 Pandora", "Pandora_Engine", "mod", "rdf"))
    copy(os.path.join(ROOT, "behavior", "Nemesis_Engine", "mod", "rdfm"), os.path.join(STAGE, "11 Nemesis", "Nemesis_Engine", "mod", "rdfm"))
    if not args.no_greatsword:
        copy(os.path.join(MODS, "AI - RDF Dodge Set - Greatsword", "meshes"), os.path.join(STAGE, "20 Greatsword Set", "meshes"))

    preset_dir = os.path.join(STAGE, "30 Preset Committed", "SKSE", "Plugins")
    os.makedirs(preset_dir)
    text = preset_ini(open(base_ini, encoding="utf-8", newline="").read(), os.path.join(ROOT, "dist", "presets", "committed-dodge.ini"))
    with open(os.path.join(preset_dir, "ReactiveDodgeFramework.ini"), "w", encoding="utf-8", newline="") as f:
        f.write(text)

    fomod = os.path.join(STAGE, "fomod")
    os.makedirs(fomod)
    with open(os.path.join(fomod, "info.xml"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(INFO.format(name=NAME, version=ver))
    with open(os.path.join(fomod, "ModuleConfig.xml"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(CONFIG.format(name=NAME, sets="" if args.no_greatsword else SETS_GROUP))

    archive = os.path.join(ROOT, "_work", "fomod", "ReactiveDodgeFramework-%s.zip" % ver)
    if os.path.exists(archive):
        os.remove(archive)
    count = 0
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dp, dn, fn in os.walk(STAGE):
            for f in sorted(fn):
                full = os.path.join(dp, f)
                z.write(full, os.path.relpath(full, STAGE))
                count += 1
    print("%s: %d files, %.1f MB" % (archive, count, os.path.getsize(archive) / 1e6))
    if not args.no_copy and os.path.isdir(DOWNLOADS):
        shutil.copy2(archive, DOWNLOADS)
        print("copied to", DOWNLOADS)
    return 0


if __name__ == "__main__":
    sys.exit(main())

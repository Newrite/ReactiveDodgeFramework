#!/usr/bin/env python3
r"""Writes the Nemesis version of the RDF behavior patch from the Pandora one (the single source of truth).

    python tools/gen_rdf_patch.py      # first: the Pandora patch
    python tools/gen_rdf_nemesis.py    # -> behavior/Nemesis_Engine/mod/rdfm

Pandora patch ops -> Nemesis files:
  <append path="#ID/param">items</append>   #ID.txt: the node's ORIGINAL text with the items in a
                                             <!-- MOD_CODE ~rdfm~ OPEN --> ... <!-- CLOSE --> block before the param's end
  <insert path="">nodes</insert>             one #rdfm$N.txt per new node
$eventID[..]$ / $variableID[..]$ are Nemesis syntax as well.

A node's original text has to be Nemesis' own, line for line (Nemesis merges by lines, and its text differs from an
hkxcmd dump: no SERIALIZE_IGNORED comments, one pointer per line in vanilla pointer arrays). It is taken from
Nemesis mods installed in this modlist that edit the same nodes, with their MOD_CODE blocks removed; two mods
must agree on it. Nemesis recounts numelements, so the originals keep their counts (as every Nemesis mod does).
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PANDORA = os.path.join(ROOT, "behavior", "Pandora_Engine", "mod", "rdf", "patches")
OUT = os.path.join(ROOT, "behavior", "Nemesis_Engine", "mod", "rdfm")
MODS = r"F:\STB\Skyrim\[STB] Mod Organizer\mods"
PANDORA_CODE, CODE = "rdf", "rdfm"

# Pandora patch file -> Nemesis behavior folder
FILES = {
    "0_master.xml": "0_master",
    "firstperson~0_master.xml": os.path.join("_1stperson", "0_master"),
    "defaultmale_character.xml": "defaultmale",
    "defaultfemale_character.xml": "defaultfemale",
}
INFO = "name=Reactive Dodge Framework\nauthor=newrite\nsite=https://www.nexusmods.com/skyrimspecialedition\n"


# ------------------------------------------------------------------------------------------- originals
def strip_mod_blocks(text):
    """A Nemesis node file without any mod's edits: OPEN..CLOSE dropped, OPEN..ORIGINAL..CLOSE -> the ORIGINAL part."""
    out, state = [], None  # None | "new" | "orig"
    for line in text.splitlines(keepends=True):
        s = line.strip()
        if s.startswith("<!-- MOD_CODE ") and s.endswith("OPEN -->"):
            state = "new"
            continue
        if s == "<!-- ORIGINAL -->" and state == "new":
            state = "orig"
            continue
        if s == "<!-- CLOSE -->" and state:
            state = None
            continue
        if state != "new":
            out.append(line)
    return "".join(out)


def find_originals(folder, node_file):
    """[(mod code, original text)] from every installed Nemesis mod that edits this node."""
    found = []
    for mod in sorted(os.listdir(MODS)):
        base = os.path.join(MODS, mod, "Nemesis_Engine", "mod")
        if not os.path.isdir(base):
            continue
        for code in sorted(os.listdir(base)):
            if code == CODE:
                continue
            p = os.path.join(base, code, folder, node_file)
            if os.path.isfile(p):
                found.append((code, strip_mod_blocks(open(p, encoding="utf-8", errors="strict").read())))
    return found


# vanilla graphs dumped by hkxcmd from Nemesis' own cached_behaviors (tools/dump_pandora_out.py --vanilla)
VANILLA = {
    "0_master": os.path.join(ROOT, "_work", "vanilla_nemesis_xml", "0_master_nem.xml"),
    "defaultmale": os.path.join(ROOT, "_work", "vanilla_nemesis_xml", "defaultmale_nem.xml"),
    "defaultfemale": os.path.join(ROOT, "_work", "vanilla_nemesis_xml", "defaultfemale_nem.xml"),
    os.path.join("_1stperson", "0_master"): os.path.join(ROOT, "_work", "fp_nem_xml", "0_master_nem.xml"),
}


def nemesis_layout(text):
    """hkxcmd text -> Nemesis' line layout: no SERIALIZE_IGNORED comments, one pointer per line, one vector component per line."""
    out = []
    for line in text.splitlines():
        if re.match(r"^\t*<!-- .* SERIALIZE_IGNORED -->$", line):
            continue
        m = re.match(r"^(\t+)(#\d{4}(?: #\d{4})+)$", line)
        if m:
            out += [m.group(1) + r for r in m.group(2).split()]
            continue
        m = re.match(r"^(\t+)\(([-\d. ]+)\)$", line)
        if m:
            out += [m.group(1) + v for v in m.group(2).split()]
            continue
        out.append(line)
    return "\n".join(out) + "\n"


def vanilla_node(folder, node_id):
    t = open(VANILLA[folder], encoding="utf-8").read()
    i = t.find('<hkobject name="%s"' % node_id)
    if i < 0:
        sys.exit("%s not in %s" % (node_id, VANILLA[folder]))
    start = t.rfind("\n", 0, i) + 1
    end = t.find("\n\t\t</hkobject>", i) + len("\n\t\t</hkobject>")
    return nemesis_layout(t[start:end])


def without_counts(text):
    return re.sub(r'numelements="\d+"', 'numelements=""', text).strip("\r\n")


def original(folder, node_id):
    """The node as Nemesis holds it: the vanilla dump in Nemesis' layout, confirmed by an installed Nemesis mod
    that edits the same node (counts aside: mods often update numelements, Nemesis recounts anyway)."""
    text = vanilla_node(folder, node_id)
    agree = [code for code, t in find_originals(folder, node_id + ".txt") if without_counts(t) == without_counts(text)]
    if not agree:
        sys.exit("%s/%s: no installed Nemesis mod confirms the original text; check nemesis_layout()" % (folder, node_id))
    return text, agree


# ------------------------------------------------------------------------------------------- formatting
def fmt(elem, depth):
    """Nemesis / hkxcmd layout: tabs, one element per line, leaf elements on one line."""
    tab = "\t" * depth
    attrs = "".join(' %s="%s"' % (k, v) for k, v in elem.attrib.items())
    children = list(elem)
    text = (elem.text or "").strip()
    if children:
        lines = ["%s<%s%s>" % (tab, elem.tag, attrs)]
        for c in children:
            lines += fmt(c, depth + 1)
        lines.append("%s</%s>" % (tab, elem.tag))
        return lines
    if text and "numelements" in elem.attrib:  # pointer / value list: on its own line
        return ["%s<%s%s>" % (tab, elem.tag, attrs), "\t" * (depth + 1) + " ".join(text.split()), "%s</%s>" % (tab, elem.tag)]
    return ["%s<%s%s>%s</%s>" % (tab, elem.tag, attrs, text, elem.tag)]


def rename(text):
    return text.replace("#%s$" % PANDORA_CODE, "#%s$" % CODE)


def parse_patch(path):
    """[(op, path, [elements or ref text])] in file order."""
    root = ET.fromstring(rename(open(path, encoding="utf-8").read()))
    ops = []
    for op in root:
        items = list(op)
        if not items and (op.text or "").strip():
            items = (op.text or "").split()  # appended pointers, e.g. states
        ops.append((op.tag, op.get("path"), items))
    return ops


def item_lines(item, depth):
    return ["\t" * depth + item] if isinstance(item, str) else fmt(item, depth)


def insert_block(node_text, param, lines):
    """Put `lines` in a MOD_CODE block just before the closing line of hkparam `param`."""
    src = node_text.splitlines(keepends=True)
    open_re = re.compile(r'^(\t*)<hkparam name="%s"' % re.escape(param))
    for n, line in enumerate(src):
        m = open_re.match(line)
        if not m:
            continue
        if line.rstrip().endswith("</hkparam>"):
            sys.exit("param %s is a one-line element in the original; not supported" % param)
        close = m.group(1) + "</hkparam>"
        for k in range(n + 1, len(src)):
            if src[k].rstrip("\r\n") == close:
                block = ["<!-- MOD_CODE ~%s~ OPEN -->\n" % CODE] + [l + "\n" for l in lines] + ["<!-- CLOSE -->\n"]
                return "".join(src[:k] + block + src[k:])
        sys.exit("no closing line for param %s" % param)
    sys.exit("param %s not in the node" % param)


# ------------------------------------------------------------------------------------------- main
def main():
    if os.path.isdir(OUT):
        for dp, dn, fn in os.walk(OUT, topdown=False):
            for f in fn:
                os.remove(os.path.join(dp, f))
            for d in dn:
                os.rmdir(os.path.join(dp, d))
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "info.ini"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(INFO)

    summary = []
    for patch, folder in FILES.items():
        ops = parse_patch(os.path.join(PANDORA, patch))
        out_dir = os.path.join(OUT, folder)
        os.makedirs(out_dir, exist_ok=True)
        edits = {}  # node id -> {param: [lines]} (param order kept)
        new_nodes = 0
        for op, path, items in ops:
            if op == "insert":
                for node in items:
                    with open(os.path.join(out_dir, node.get("name") + ".txt"), "w", encoding="utf-8", newline="\r\n") as f:  # CRLF like Nemesis mods
                        f.write("\n".join(fmt(node, 2)) + "\n")
                    new_nodes += 1
            elif op == "append":
                node_id, param = path.split("/", 1)
                lines = edits.setdefault(node_id, {}).setdefault(param, [])
                for it in items:
                    lines += item_lines(it, 4)
            else:
                sys.exit("unsupported Pandora op %s in %s" % (op, patch))
        sources = {}
        for node_id, params in edits.items():
            text, codes = original(folder, node_id)
            sources[node_id] = codes
            for param, lines in params.items():
                text = insert_block(text, param, lines)
            with open(os.path.join(out_dir, node_id + ".txt"), "w", encoding="utf-8", newline="\r\n") as f:
                f.write(text)
        summary.append("%s: %d new nodes, edited %s (originals from %s)"
                       % (folder, new_nodes, sorted(edits), {k: v[:2] for k, v in sources.items()}))
    print("\n".join(summary))
    print("wrote", OUT)


if __name__ == "__main__":
    main()

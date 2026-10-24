#!/usr/bin/env python3
"""Writes docs/ru/Settings.md: every setting of the FLICK menu (F7) with its ini section and key, the page it is on,
and its Russian label and description, read from src/game/SettingsSchema.cpp and tools/gen_menu_translations.py (the
same texts the menu shows). Settings only in the ini (not in the menu) are listed from the shipped ini.

    python tools/gen_settings_reference.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_menu_translations as tr  # noqa: E402

SCHEMA = os.path.join(ROOT, "src", "game", "SettingsSchema.cpp")
INI = os.path.join(ROOT, "dist", "ReactiveDodgeFramework.ini")
OUT = os.path.join(ROOT, "docs", "ru", "Settings.md")
PAGES = ["Input", "Timing", "Motion", "Steering", "Animation", "Rules", "Debug"]


def ru(key):
    v = tr.T.get(key)
    return v[1] if v else ""


def schema_fields():
    src = open(SCHEMA, encoding="utf-8").read()
    fields = []
    # Float / Int / Bool / Form (Page::X, "Section", "Key", ...), optionally wrapped in Named(..., "TextKey")
    for m in re.finditer(r'(Named\()?(?:Custom\()?(?:Float|Int|Bool|Form|Color)\(Page::(\w+), "(\w+)", "(\w+)"[^\n]*?(?:, "(\w+)"\))?,?\n', src):
        named, page, section, key, text = m.groups()
        fields.append((page, section, key, text if named and text else key, m.start()))
    # Choice fields: Field x{ Page::X, "Section", "Key", Kind::Choice } inside their helper; placed where the helper is called
    helpers = {}
    for m in re.finditer(r'Field (\w+)\(\)\s*\{\s*Field x\{ Page::(\w+), "(\w+)", "(\w+)", Kind::Choice \}', src):
        helpers[m.group(1)] = (m.group(2), m.group(3), m.group(4))
    table = src.index("const std::vector<Field>& Table()")
    for name, (page, section, key) in helpers.items():
        call = src.find("\t" + name + "(),", table)
        if call >= 0:
            fields.append((page, section, key, key, call))
    fields.sort(key=lambda f: f[4])
    return [f[:4] for f in fields if f[4] > table]


def ini_keys():
    keys, section = [], None
    for line in open(INI, encoding="utf-8"):
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif "=" in line and not line.startswith(";"):
            keys.append((section, line.split("=", 1)[0].strip()))
    return keys


def choices(key):
    out = []
    for k, v in tr.T.items():
        if k.startswith(key + "_") and not k.endswith("_Desc"):
            out.append("`%s` = %s" % (k[len(key) + 1:], v[1]))
    return out


def main():
    fields = schema_fields()
    menu = {(s, k) for _, s, k, _ in fields}
    lines = [
        "# Настройки Reactive Dodge Framework",
        "",
        "Справочник всех настроек: как ключ называется в `SKSE/Plugins/ReactiveDodgeFramework.ini` и как он подписан в меню",
        "FLICK (F7, группа «Reactive Dodge»). Подписи и описания те же, что показывает меню. Файл собирается",
        "автоматически (`tools/gen_settings_reference.py`); правьте тексты в `tools/gen_menu_translations.py`.",
        "",
    ]
    for page in PAGES:
        rows = [f for f in fields if f[0] == page]
        if not rows:
            continue
        lines += ["## Страница «%s»" % ru("Page_" + page), "", "| В ini | В меню F7 | Что делает |", "|---|---|---|"]
        for _, section, key, text in rows:
            desc = ru(text + "_Desc").replace("|", "\\|")
            extra = choices(text)
            if extra:
                desc += " Значения: " + "; ".join(extra) + "."
            lines.append("| `[%s] %s` | %s | %s |" % (section, key, ru(text) or "—", desc))
        lines.append("")
    rest = [(s, k) for s, k in ini_keys() if (s, k) not in menu]
    if rest:
        lines += ["## Только в ini", "", "Эти ключи меню не показывает (хуки, события графа, совместимость): они действуют со старта",
                  "игры, их описание в комментариях самого ini.", ""]
        by = {}
        for s, k in rest:
            by.setdefault(s, []).append("`%s`" % k)
        for s, ks in by.items():
            lines.append("- `[%s]`: %s" % (s, ", ".join(ks)))
        lines.append("")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("wrote %s: %d menu settings, %d ini-only" % (OUT, len(fields), len(rest)))
    missing = [k for _, _, _, k in fields if not ru(k)]
    if missing:
        print("no Russian label:", ", ".join(missing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

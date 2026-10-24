#!/usr/bin/env python3
"""Turns docs/ru/*.md into self-contained HTML pages (docs/ru/html) a browser opens by double-click: one page per
document, a shared top navigation, links between the documents pointing at the .html pages, light and dark themes.
No dependencies: a small converter for the Markdown the docs use (headings, paragraphs, nested lists, tables, code
fences, inline code, bold, links).

    python tools/gen_docs_html.py
"""
import html
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "docs", "ru")
OUT = os.path.join(SRC, "html")
# page order and the label in the navigation; README becomes index.html
PAGES = [("README.md", "index.html", "Обзор"), ("Annotations.md", "Annotations.html", "Аннотации"),
         ("Keywords.md", "Keywords.html", "Кейворды"), ("CastOn.md", "CastOn.html", "CastOn"),
         ("Settings.md", "Settings.html", "Настройки")]
LINKS = {src: out for src, out, _ in PAGES}

CSS = """
:root { --bg: #ffffff; --fg: #1d1f23; --muted: #5b6270; --line: #d9dde3; --code: #f2f4f7; --accent: #2f6fd6; --head: #f6f7f9; }
@media (prefers-color-scheme: dark) {
  :root { --bg: #16181c; --fg: #e3e6eb; --muted: #9aa3b2; --line: #2e333b; --code: #22262d; --accent: #7aa7ff; --head: #1d2026; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.55 "Segoe UI", system-ui, sans-serif; }
nav { position: sticky; top: 0; background: var(--head); border-bottom: 1px solid var(--line); padding: 10px 16px;
      display: flex; flex-wrap: wrap; gap: 6px 18px; align-items: center; z-index: 1; }
nav .title { font-weight: 600; margin-right: 8px; }
nav a { color: var(--accent); text-decoration: none; }
nav a.here { color: var(--fg); font-weight: 600; }
main { max-width: 1100px; margin: 0 auto; padding: 8px 16px 48px; }
h1 { font-size: 1.7em; margin: 0.8em 0 0.4em; }
h2 { font-size: 1.3em; margin: 1.6em 0 0.5em; padding-bottom: 4px; border-bottom: 1px solid var(--line); }
h3 { font-size: 1.1em; margin: 1.3em 0 0.4em; }
a { color: var(--accent); }
code { background: var(--code); padding: 1px 5px; border-radius: 4px; font: 0.9em Consolas, "Cascadia Mono", monospace; }
pre { background: var(--code); padding: 12px 14px; border-radius: 6px; overflow-x: auto; }
pre code { padding: 0; background: none; }
.table { overflow-x: auto; margin: 0.8em 0; }
table { border-collapse: collapse; width: 100%; font-size: 0.95em; }
th, td { border: 1px solid var(--line); padding: 6px 9px; text-align: left; vertical-align: top; }
th { background: var(--head); }
li { margin: 3px 0; }
li > ul, li > ol { margin: 3px 0; }
footer { color: var(--muted); font-size: 0.85em; margin-top: 3em; }
"""


def inline(text):
    """Inline Markdown: code spans become placeholders (kept verbatim), then bold and links on the escaped rest, so
    bold may wrap code (**`x`**)."""
    codes = []

    def keep(m):
        codes.append("<code>%s</code>" % html.escape(m.group(1)))
        return "\x00%d\x00" % (len(codes) - 1)

    s = html.escape(re.sub(r"`([^`]*)`", keep, text), quote=False)
    s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)

    def link(m):
        target = m.group(2)
        base, _, anchor = target.partition("#")
        label = m.group(1)
        if base in LINKS:
            target = LINKS[base] + ("#" + anchor if anchor else "")
            label = LINKS[base] if label == base else label  # "Settings.md" reads "Settings.html" on the page
        return '<a href="%s">%s</a>' % (html.escape(target), label)

    s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, s)
    return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], s)


def table(lines):
    rows = [[c.strip() for c in re.split(r"(?<!\\)\|", ln.strip().strip("|"))] for ln in lines]
    head, body = rows[0], [r for r in rows[2:]]
    cell = lambda c: inline(c.replace("\\|", "|"))
    out = ["<div class=\"table\"><table><thead><tr>"] + ["<th>%s</th>" % cell(c) for c in head] + ["</tr></thead><tbody>"]
    for r in body:
        out.append("<tr>" + "".join("<td>%s</td>" % cell(c) for c in r) + "</tr>")
    out.append("</tbody></table></div>")
    return "".join(out)


ITEM = re.compile(r"^( *)(- |\d+\. )(.*)$")


def lists(lines):
    """Nested lists by indentation: an item is a marker line plus the more-indented lines after it."""
    out, stack = [], []  # stack of (indent, tag)
    item_open = False
    for ln in lines:
        m = ITEM.match(ln)
        if m:
            indent, tag, text = len(m.group(1)), ("ol" if m.group(2)[0].isdigit() else "ul"), m.group(3)
            while stack and stack[-1][0] > indent:
                out.append("</li></%s>" % stack.pop()[1])
            if stack and stack[-1][0] == indent:
                out.append("</li>")
            else:
                out.append("<%s>" % tag)
                stack.append((indent, tag))
            out.append("<li>" + inline(text))
            item_open = True
        elif item_open:
            out.append(" " + inline(ln.strip()))  # a continuation line of the item
    while stack:
        out.append("</li></%s>" % stack.pop()[1])
    return "".join(out)


def convert(md):
    lines = md.replace("\r\n", "\n").split("\n")
    out, i, title = [], 0, None
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("```"):
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            out.append("<pre><code>%s</code></pre>" % html.escape("\n".join(lines[i + 1:j])))
            i = j + 1
            continue
        m = re.match(r"^(#{1,4}) (.*)$", ln)
        if m:
            level, text = len(m.group(1)), m.group(2)
            title = title or text
            anchor = re.sub(r"[^\w\- ]", "", text.lower()).strip().replace(" ", "-")
            out.append('<h%d id="%s">%s</h%d>' % (level, html.escape(anchor), inline(text), level))
            i += 1
            continue
        if ln.lstrip().startswith("|"):
            j = i
            while j < len(lines) and lines[j].lstrip().startswith("|"):
                j += 1
            out.append(table(lines[i:j]))
            i = j
            continue
        if ITEM.match(ln):
            j = i + 1
            while j < len(lines) and lines[j].strip() and (ITEM.match(lines[j]) or lines[j].startswith(" ")):
                j += 1
            # a blank line inside a list continues it when the next line is still an item or indented
            while j + 1 < len(lines) and not lines[j].strip() and (ITEM.match(lines[j + 1]) or lines[j + 1].startswith("  ")):
                j += 1
                while j < len(lines) and lines[j].strip() and (ITEM.match(lines[j]) or lines[j].startswith(" ")):
                    j += 1
            block = [l for l in lines[i:j] if l.strip()]
            # a table or code inside an item (indented) is rendered after the list
            out.append(lists([l for l in block if not l.lstrip().startswith("|")]))
            rows = [l for l in block if l.lstrip().startswith("|")]
            if rows:
                out.append(table(rows))
            i = j
            continue
        if not ln.strip():
            i += 1
            continue
        j = i
        while j < len(lines) and lines[j].strip() and not re.match(r"^(#|```|\s*\||- |\d+\. )", lines[j]):
            j += 1
        out.append("<p>%s</p>" % inline(" ".join(l.strip() for l in lines[i:j])))
        i = j
    return title or "", "\n".join(out)


def main():
    os.makedirs(OUT, exist_ok=True)
    for src, dst, _ in PAGES:
        path = os.path.join(SRC, src)
        if not os.path.exists(path):
            sys.exit("missing %s" % path)
        title, body = convert(open(path, encoding="utf-8").read())
        nav = "".join('<a href="%s"%s>%s</a>' % (out, ' class="here"' if out == dst else "", label) for _, out, label in PAGES)
        page = ('<!doctype html>\n<html lang="ru"><head><meta charset="utf-8">'
                '<meta name="viewport" content="width=device-width, initial-scale=1">'
                "<title>%s</title><style>%s</style></head><body>"
                '<nav><span class="title">Reactive Dodge Framework</span>%s</nav><main>%s'
                "<footer>Собрано из %s (tools/gen_docs_html.py).</footer></main></body></html>\n"
                % (html.escape(title), CSS, nav, body, src))
        open(os.path.join(OUT, dst), "w", encoding="utf-8", newline="\n").write(page)
    print("wrote %d pages to %s" % (len(PAGES), OUT))


if __name__ == "__main__":
    main()

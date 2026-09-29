#!/usr/bin/env python3
"""Regenerates the "Built-in functions" section of docs/SCRIPTING.md from the
natives tables in engine/script/*.c (run after adding or changing a function)."""
import re, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rows = []
for f in ['engine/script/a3_script_lib.c', 'engine/script/a3_script_engine.c']:
    s = open(os.path.join(root, f)).read()
    for m in re.finditer(r'\{ "(\w+)", n_\w+, (-?\d+), (-?\d+), "([^"]+)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)" \}', s):
        rows.append((m.group(4), m.group(5).replace('\\"', '"'), m.group(6).replace('\\"', '"')))
cats = []
for r in rows:
    if r[0] not in cats:
        cats.append(r[0])
out = []
for c in cats:
    out.append("\n### %s\n\n| Function | What it does |\n|---|---|" % c)
    for r in rows:
        if r[0] == c:
            out.append("| `%s` | %s |" % (r[1], r[2].replace('|', '\\|')))
marker = "## Built-in functions"
path = os.path.join(root, 'docs/SCRIPTING.md')
doc = open(path).read()
head = doc[:doc.index(marker)]
intro = "%s\n\nAlso listed in the editor under **Docs > Script API** (%d functions). `pi` and `tau` are\nconstants.\n" % (marker, len(rows))
open(path, 'w').write(head + intro + "\n".join(out) + "\n")
print("%d functions in %d categories" % (len(rows), len(cats)))

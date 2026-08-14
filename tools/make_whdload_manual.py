#!/usr/bin/env python3
"""Render docs/manual.md into the WHDLoad install's plain-text `Manual`.

The install package ships the original Lucasfilm manual as a text file that
MultiView/More can open on a stock 1.3 machine, so the output is deliberately
dumb: ISO-8859-1 (in practice pure ASCII -- no smart quotes, em dashes or
(TM)/(C) glyphs), one leading space per line, hard-wrapped to 76 columns so it
fits an 80-column Shell window with room for the border.

Run from the repo root after editing docs/manual.md:

    python3 tools/make_whdload_manual.py

The result is checked in (whdload/RoF Install/Manual) -- create_release.sh only
copies the package, it does not run this.
"""

import re, textwrap

lines = open("docs/manual.md", encoding="utf-8").read().split("\n")


def deuni(s):
    for k, v in {"’":"'", "‘":"'", "“":'"', "”":'"',
                 "—":"--", "–":"-", "…":"...", "™":"(TM)",
                 "©":"(C)", "×":"x", "°":" degrees",
                 "→":"->", " ":" "}.items():
        s = s.replace(k, v)
    return s

def inline(s):
    s = deuni(s)
    s = re.sub(r"\*\*(.+?)\*\*", r"\1", s)
    s = re.sub(r"\*(.+?)\*", r"\1", s)
    return s

W = 76
out = []

def nblank(n):                       # exactly n blank lines before the next text
    while out and out[-1] == "":
        out.pop()
    if out:
        out.extend([""] * n)

def para(text, indent=" ", hang=None):
    out.extend(textwrap.wrap(text, width=W, initial_indent=indent,
                             subsequent_indent=indent if hang is None else hang,
                             break_on_hyphens=False))

SCORE_ROWS = [("Event", "Point Value"), None,
              ("Each second of flight", "1"),
              ("Gun emplacement destroyed", "100"),
              ("Saucer destroyed", "250"),
              ("Pilot picked up", "200"),
              ("Ace picked up", "2000"),
              ("Pilot returned to Mother Ship (bonus)", "500"),
              ("Pilot returned over quota (bonus)", "1000"),
              ("Level completed", "Level x 200")]
COL = 46                                   # right edge of the value column
SCORING = []
for row in SCORE_ROWS:
    if row is None:
        SCORING.append(" " + "-" * 5 + " " * (COL - 5 - 11) + "-" * 11)
        continue
    ev, pv = row
    SCORING.append(" " + ev + " " * (COL - len(ev) - len(pv)) + pv)

i = 0
while i < len(lines):
    ln = lines[i].rstrip()
    if ln == "":
        i += 1
        continue

    m = re.match(r"^(#+)\s+(.*)$", ln)
    if m:
        lvl, title = len(m.group(1)), inline(m.group(2)).strip()
        if i == 0:                                   # the cover block
            name = title.replace("(TM)", "")
            head = name.upper() + "(TM)"
            out.append(" " + head)
            out.append(" " + "-" * len(head))
            out.append("")
            out.append(" Lucasfilm Games -- DYAP07")
            while i < len(lines) and not lines[i].startswith("# W"):
                i += 1                               # skip the cover's own lines
            continue
        nblank(2 if lvl == 1 else 1)
        if lvl == 1:
            out.append(" " + title)
            out.append(" " + "-" * len(title))
        else:
            out.append(" " + title + ":")
        out.append("")
        i += 1
        continue

    if ln.startswith("|"):                           # the one table
        while i < len(lines) and lines[i].startswith("|"):
            i += 1
        nblank(1)
        out.extend(SCORING)
        out.append("")
        continue

    if ln.startswith("- "):                          # the joystick list
        nblank(1)
        while i < len(lines) and lines[i].startswith("- "):
            para(inline(lines[i][2:].rstrip()), indent="\t", hang="\t  ")
            i += 1
        out.append("")
        continue

    txt = inline(ln).strip()
    nblank(1)
    n = re.match(r"^(\d+\.)\s", txt)                 # CODE RED! steps: hang-indent
    para(txt, hang="    " if n else None)
    i += 1

text = "\n".join(out)
text = re.sub(r"\n{4,}", "\n\n\n", text).strip("\n") + "\n"
open("whdload/RoF Install/Manual", "w", encoding="latin-1").write(text)
print(text.count("\n"), "lines, longest",
      max(len(l) for l in text.split("\n")),
      "non-ascii:", {c for c in text if ord(c) > 126})

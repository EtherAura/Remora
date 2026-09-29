#!/usr/bin/env python3
"""dedupe-res.py — drop duplicate-named entries from Android resource XML. Arg: <project-dir>

WHY. The LineageOS backport applies ~98 patches that all append to the same handful of resource
files (res/values/cm_strings.xml alone is touched by 27 of them). Applying them with git's default
merge leaves those files in conflict and costs ~24 patches. A `merge=union` driver fixes that — it
takes both sides instead of conflicting — but union merge is textual and has no idea it is looking
at XML: where two patches carry the same block in their context, it happily keeps both copies.

Measured on the Settings series: union merge took the series from 52/98 to 76/98 applied, and left
6 duplicate <string> names (all fingerprint_enroll_*, duplicated out of the context regions of the
refresh-rate and translation-import patches). aapt2 treats a duplicate resource as a hard error
("resource string/x already defined"), so the build would fail at packaging with an error that
says nothing about patch application.

This runs after the series and keeps the LAST definition of each (tag, name), which matches how
the patches were meant to stack: a later patch that redefines a string wins.

Idempotent, and a no-op for projects with no res/values.
"""
import glob
import os
import sys
import xml.etree.ElementTree as ET

proj = sys.argv[1] if len(sys.argv) > 1 else "."
changed_total = 0

for path in sorted(glob.glob(os.path.join(proj, "res", "values*", "*.xml"))):
    try:
        tree = ET.parse(path)
    except ET.ParseError:
        continue  # not our business — let the compiler complain about malformed XML
    root = tree.getroot()
    if root.tag != "resources":
        continue

    # keep the LAST occurrence of each (tag, name)
    seen_last = {}
    for idx, el in enumerate(list(root)):
        name = el.get("name")
        if name is None:
            continue
        seen_last[(el.tag, name)] = idx

    drop = []
    for idx, el in enumerate(list(root)):
        name = el.get("name")
        if name is None:
            continue
        if seen_last[(el.tag, name)] != idx:
            drop.append(el)

    if not drop:
        continue
    for el in drop:
        root.remove(el)
    tree.write(path, encoding="utf-8", xml_declaration=True)
    changed_total += len(drop)
    print("  dedupe-res: %s — dropped %d duplicate entr%s"
          % (os.path.relpath(path, proj), len(drop), "y" if len(drop) == 1 else "ies"))

if changed_total == 0:
    print("  dedupe-res: no duplicate resources")

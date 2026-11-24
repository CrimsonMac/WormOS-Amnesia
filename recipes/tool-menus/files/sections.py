"""Print Kali's menu sections as "CATEGORY = ORDER|NAME|ICON" lines.

Each top-level menu of kali-applications.menu is a section; every
category it includes, at any depth, maps to it (a category in two
sections keeps the first). Kali's "Usual Applications" is left out.
"""

import os
import re
import sys
import xml.etree.ElementTree as ET

menu_file, directories = sys.argv[1:3]
root = ET.parse(menu_file).getroot()
seen = set()
order = 0
for top in root.findall("Menu"):
    name = top.findtext("Name")
    directory = top.findtext("Directory") or ""
    if not name or name == "Usual Applications":
        continue
    order += 1
    icon = ""
    path = os.path.join(directories, directory)
    if os.path.exists(path):
        m = re.search(r"^Icon=(.+)$", open(path).read(), re.M)
        icon = m.group(1).strip() if m else ""
    for cat in top.iter("Category"):
        c = (cat.text or "").strip()
        if c and c not in seen:
            seen.add(c)
            print(f"{c} = {order:02d}|{name}|{icon}")

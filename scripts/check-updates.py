#!/usr/bin/env python3
"""Compare every recipe's version with the newest upstream release known to
Repology (repology.org), and list the packages that are behind. CI runs it
every week and keeps an issue with the list.

Packages of WormOS's own code (no upstream source) are skipped. Repology
names some projects differently: REPOLOGY_NAMES maps them, SKIP lists
packages it cannot answer for.

Usage: scripts/check-updates.py [--markdown] [PACKAGE...]
"""

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = "https://repology.org/api/v1/project/"
AGENT = "WormOS-update-check/1 (+https://github.com/CrimsonMac/WormOS)"

REPOLOGY_NAMES = {
    "7zip": "7zip",
    "adwaita-icon-theme": "adwaita-icon-theme",
    "bind9-dnsutils": "bind",
    "font-hack": "fonts:hack",
    "font-inter": "fonts:inter",
    "gtk3": "gtk",
    "gtk4": "gtk",
    "libjpeg-turbo": "libjpeg-turbo",
    "linux": "linux",
    "linux-firmware": "linux-firmware",
    "lua": "lua",
    "python": "python",
    "tor-browser": "tor-browser",
    "tor-browser-bundle": "tor-browser",
    "xcb-proto": "xcb-proto",
    "zsh-autosuggestions": "zsh-autosuggestions",
    "zsh-syntax-highlighting": "zsh-syntax-highlighting",
}
# Branches that must stay on their own series (newest of that series counts).
SERIES = {
    "gtk3": r"^3\.",
    "lua": r"^5\.4\.",
    "python": r"^3\.13\.",
    "linux": None,  # the same major.minor (a long-term series)
    "openssl": r"^3\.5\.",  # long-term support series
    "ncdu": r"^1\.",          # 2.x is a rewrite in Zig
    "wget": r"^1\.2\d",       # Wget2 (2.x, pre-releases 1.99) is another program
}
SKIP = {"tor-browser-runtime", "wormhole-keyrings", "ca-certificates", "licenses", "tzdata"}


def recipe_info(name):
    if name == "linux":                     # scripts/build-kernel.sh, not a recipe
        text = open(os.path.join(ROOT, "scripts", "build-kernel.sh"), encoding="utf-8").read()
        return re.search(r"^LINUX=linux-(\S+)", text, re.M).group(1), True
    path = os.path.join(ROOT, "recipes", name, "recipe")
    if not os.path.isfile(path):
        return None, False
    text = open(path, encoding="utf-8").read()
    version = re.search(r"^version=(\S+)", text, re.M)
    has_source = re.search(r"^source=\(", text, re.M) and not re.search(r"^localsrc=", text, re.M)
    return (version.group(1).strip("'\"") if version else None), bool(has_source)


SEP = r"[.\-_+]|(?<=\d)p(?=\d)"


def key(v):
    # 26.06 and 26.6.0 are the same version: numbers, without trailing zeros.
    # 5.3p20 (a GNU patch level) is our 5.3.20.
    parts = [int(p) if p.isdigit() else p for p in re.split(SEP, v)]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return parts


def newer(a, b):
    try:
        return key(a) > key(b)
    except TypeError:
        return a != b and a > b


def newest(project, series, ours):
    req = urllib.request.Request(API + project, headers={"User-Agent": AGENT})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = json.load(r)
            break
        except (urllib.error.URLError, TimeoutError):
            time.sleep(5 * (attempt + 1))
    else:
        return None
    # A version counts once two repositories have it (one odd repository's
    # numbering, such as 8.23.0.4, does not), or if only one has it, when
    # it is shaped like ours (as many parts).
    seen = {}
    for p in data:
        v = p.get("version", "")
        if p.get("status") not in ("newest", "outdated", "legacy") or not re.match(r"^\d", v):
            continue
        if series and not re.match(series, v):
            continue
        if p.get("status") == "newest" or series:
            seen[v] = seen.get(v, 0) + 1
    shape = len(re.split(SEP, ours))
    best = None
    for v, n in seen.items():
        if (n >= 2 or len(re.split(SEP, v)) == shape) and (best is None or newer(v, best)):
            best = v
    return best


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    markdown = "--markdown" in sys.argv
    names = args or sorted(os.listdir(os.path.join(ROOT, "recipes"))) + ["linux"]
    behind, unknown = [], []
    for name in names:
        if name in SKIP or name.startswith("wormos-") or name.startswith("worm"):
            continue
        ours, upstream_src = recipe_info(name)
        if not ours or not upstream_src:
            continue
        series = SERIES.get(name)
        if name == "linux":
            series = r"^" + re.escape(".".join(ours.split(".")[:2])) + r"\."
        theirs = newest(REPOLOGY_NAMES.get(name, name), series, ours)
        time.sleep(1.1)                     # Repology asks for one request a second
        if theirs is None:
            unknown.append(name)
        elif newer(theirs, ours):
            behind.append((name, ours, theirs))
        if not markdown:
            print(f"{name:28} {ours:16} {theirs or '?'}", flush=True)
    if markdown:
        print(f"{len(behind)} packages have newer upstream releases (from Repology).\n")
        if behind:
            print("| Package | Ours | Newest |\n|---|---|---|")
            for name, ours, theirs in behind:
                print(f"| {name} | {ours} | {theirs} |")
        if unknown:
            print(f"\nNot found on Repology: {', '.join(unknown)}.")
    else:
        print(f"\n{len(behind)} behind, {len(unknown)} unknown")
    return 0


if __name__ == "__main__":
    sys.exit(main())

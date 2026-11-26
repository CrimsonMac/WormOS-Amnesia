#!/usr/bin/env python3
"""Turn a test's output into a GitHub Actions job summary.

The tests print one line per check ("PASS  name" or "FAIL  name", the
failing output on the next line) and a total ("12/12 checks passed", or
"sha256   15/15 checks passed" for the unit tests). This writes a short
Markdown report to $GITHUB_STEP_SUMMARY (or to standard output outside
GitHub): the totals, every failed check with its output, and the passed
checks folded away.

Usage: scripts/ci-summary.py TITLE OUTPUT-FILE
"""

import os
import re
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    title, path = sys.argv[1], sys.argv[2]
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError as e:
        lines = [f"FAIL  no test output ({e.strerror})"]

    passed, failed, totals = [], [], []
    for i, line in enumerate(lines):
        if line.startswith("PASS  "):
            passed.append(line[6:])
        elif line.startswith("FAIL  "):
            detail = ""
            if i + 1 < len(lines) and lines[i + 1].startswith("      output:"):
                detail = lines[i + 1].strip()[len("output:"):].strip()
            failed.append((line[6:], detail))
        elif re.search(r"\b\d+/\d+ checks passed$", line):
            totals.append(line.strip())

    ok = not failed and totals and all(
        re.search(r"(\d+)/(\1) checks passed$", t) for t in totals)
    out = [f"### {'✅' if ok else '❌'} {title}", ""]
    out += [f"- {t}" for t in totals] or ["- no total: the test stopped early"]
    out.append("")
    if failed:
        out += ["| Failed check | Output |", "| --- | --- |"]
        for name, detail in failed:
            detail = detail.replace("|", "\\|")
            if len(detail) > 300:
                detail = detail[:300] + "…"
            out.append(f"| {name} | `{detail}` |" if detail else f"| {name} | |")
        out.append("")
    if passed:
        out += [f"<details><summary>{len(passed)} passed checks</summary>", ""]
        out += [f"- {name}" for name in passed]
        out += ["", "</details>", ""]

    target = os.environ.get("GITHUB_STEP_SUMMARY")
    if target:
        with open(target, "a", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
    else:
        print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())

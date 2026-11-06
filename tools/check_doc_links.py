#!/usr/bin/env python3
"""Fail when a Markdown file links to a repository path that does not exist.

    python tools/check_doc_links.py            # check every tracked *.md
    python tools/check_doc_links.py --list     # also print each broken link

Only relative links are checked: external URLs, mailto: and pure #anchors are
skipped, and a #fragment on a file link is ignored (the file must exist; the
heading is not verified). Links inside fenced code blocks are ignored.

Run by CI so that a moved or deleted file cannot silently strand the docs that
cite it.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"!?\[(?:[^\]\[]|\[[^\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
FENCE = re.compile(r"^\s*(```|~~~)")
SKIP_PREFIXES = ("http://", "https://", "mailto:", "#", "data:")


def tracked_markdown() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "*.md"],
        cwd=ROOT, check=True, capture_output=True,
    ).stdout.decode("utf-8")
    return [ROOT / p for p in out.split("\0") if p and (ROOT / p).is_file()]


def broken_links(md: Path) -> list[tuple[int, str]]:
    problems = []
    in_fence = False
    for number, line in enumerate(md.read_text(encoding="utf-8").splitlines(), 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for match in LINK.finditer(line):
            target = match.group(1)
            if target.startswith(SKIP_PREFIXES):
                continue
            path = unquote(target.split("#", 1)[0])
            if not path:
                continue
            resolved = (ROOT / path.lstrip("/")) if path.startswith("/") else (md.parent / path)
            if not resolved.exists():
                problems.append((number, target))
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true", help="print every broken link")
    args = parser.parse_args()

    total = 0
    files = 0
    for md in sorted(tracked_markdown()):
        problems = broken_links(md)
        if not problems:
            continue
        files += 1
        total += len(problems)
        rel = md.relative_to(ROOT).as_posix()
        if args.list:
            for number, target in problems:
                print(f"{rel}:{number}: {target}")
        else:
            print(f"{rel}: {len(problems)} broken link(s)")
    if total:
        print(f"\n{total} broken link(s) in {files} file(s)", file=sys.stderr)
        return 1
    print("doc links OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

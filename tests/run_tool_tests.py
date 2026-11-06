#!/usr/bin/env python3
"""Run every Python, JavaScript and PowerShell test under tests/tools/.

    python tests/run_tool_tests.py            # all of them
    python tests/run_tool_tests.py motion     # only files whose name contains "motion"

Each test file is a standalone program that exits non-zero on failure, so this
runner only discovers, runs and summarises them. Some Python tests compile a
small C fixture and need gcc on PATH, exactly as tests/run_host_tests.ps1 does.
JavaScript tests need Node.js 18 or newer; PowerShell tests need PowerShell 7.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL_TESTS = ROOT / "tests" / "tools"
RUNNERS = {".py": [sys.executable], ".mjs": ["node"], ".ps1": ["pwsh", "-NoProfile", "-File"]}


def discover(name_filter: str) -> list[Path]:
    return sorted(
        path for path in TOOL_TESTS.iterdir()
        if path.name.startswith("test_") and path.suffix in RUNNERS and name_filter in path.name
    )


def main() -> int:
    name_filter = sys.argv[1] if len(sys.argv) > 1 else ""
    tests = discover(name_filter)
    if not tests:
        print(f"no tool tests match {name_filter!r}", file=sys.stderr)
        return 1
    for suffix, tool in ((".mjs", "node"), (".ps1", "pwsh")):
        if any(t.suffix == suffix for t in tests) and shutil.which(tool) is None:
            print(f"{tool} is required for the {suffix} tool tests", file=sys.stderr)
            return 1

    failed = []
    for test in tests:
        started = time.monotonic()
        result = subprocess.run(
            RUNNERS[test.suffix] + [str(test)], cwd=ROOT,
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        elapsed = time.monotonic() - started
        status = "PASS" if result.returncode == 0 else "FAIL"
        print(f"{status}  {test.name}  ({elapsed:.1f}s)")
        if result.returncode != 0:
            failed.append(test.name)
            print(result.stdout[-4000:] + result.stderr[-4000:])

    print(f"\ntool tests: {len(tests) - len(failed)}/{len(tests)} passed")
    if failed:
        print("failures: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

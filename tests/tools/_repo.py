"""Shared path setup for the tool tests in this directory.

The tests exercise modules that live in tools/. Importing this module puts
tools/ on sys.path and exposes the repository root, so each test can be run
directly (``python tests/tools/test_x.py``) without any environment setup.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"

if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

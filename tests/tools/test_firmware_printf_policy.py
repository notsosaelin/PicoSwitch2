#!/usr/bin/env python3
"""printf() arguments in firmware sources must be free of side effects.

The firmware has no stdio back end, so firmware/config/ns2_no_stdio.h compiles
every project printf() call away (NS2_DISCARD_PRINTF, on by default). A removed
call does not evaluate its arguments. An argument such as `count++` or
`take_next()` would therefore run in a debug build and silently not run in the
release build.

This check is deliberately coarse: it rejects increment, decrement and
assignment operators anywhere inside a printf() call. A call to a function with
side effects cannot be detected textually; reviewers must keep those out.
"""

from __future__ import annotations

import re
import unittest

import _repo  # puts tools/ on sys.path

ROOT = _repo.ROOT
SOURCES = ROOT / "firmware" / "src"

CALL = re.compile(r"(?<![A-Za-z0-9_])printf\s*\(")
STRING = re.compile(r'"(?:\\.|[^"\\])*"')
CHAR = re.compile(r"'(?:\\.|[^'\\])'")
SIDE_EFFECT = re.compile(r"\+\+|--|(?<![=!<>+\-*/%&|^])=(?!=)")


def printf_calls(text: str):
    """Yield (offset, argument text) for each printf( ... ) call."""
    for match in CALL.finditer(text):
        depth = 1
        index = match.end()
        while index < len(text) and depth:
            char = text[index]
            if char == '"':
                end = STRING.match(text, index)
                index = end.end() if end else index + 1
                continue
            if char == "'":
                end = CHAR.match(text, index)
                index = end.end() if end else index + 1
                continue
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            index += 1
        yield match.start(), text[match.end() : index - 1]


class PrintfPolicyTests(unittest.TestCase):
    def test_header_and_option_exist(self) -> None:
        header = ROOT / "firmware" / "config" / "ns2_no_stdio.h"
        self.assertTrue(header.is_file())
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("NS2_DISCARD_PRINTF", cmake)
        self.assertIn("ns2_no_stdio.h", cmake)

    def test_printf_arguments_have_no_side_effects(self) -> None:
        offenders = []
        for path in sorted(SOURCES.rglob("*.c")):
            text = path.read_text(encoding="utf-8", errors="replace")
            for offset, arguments in printf_calls(text):
                bare = CHAR.sub("''", STRING.sub('""', arguments))
                if SIDE_EFFECT.search(bare):
                    line = text.count("\n", 0, offset) + 1
                    offenders.append(f"{path.relative_to(ROOT)}:{line}")
        self.assertEqual(
            offenders,
            [],
            "printf() arguments with ++, -- or assignment are not evaluated in "
            "the release build (ns2_no_stdio.h): " + ", ".join(offenders),
        )


if __name__ == "__main__":
    unittest.main()

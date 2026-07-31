#!/usr/bin/env python3
"""Dependency-free structural checks for PicoSwitch2's repo-local Codex skills."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SKILLS_ROOT = ROOT / ".agents" / "skills"
EXPECTED_SKILLS = {
    "picoswitch2-protocol-lab",
    "picoswitch2-motion-lab",
    "picoswitch2-firmware-tap",
    "picoswitch2-audio-regression",
}
NAME_RE = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")


def parse_frontmatter(text: str) -> dict[str, str]:
    lines = text.splitlines()
    if not lines or lines[0] != "---":
        raise ValueError("SKILL.md must start with YAML frontmatter")
    try:
        end = lines.index("---", 1)
    except ValueError as exc:
        raise ValueError("SKILL.md frontmatter is not terminated") from exc

    fields: dict[str, str] = {}
    for line in lines[1:end]:
        if not line.strip():
            continue
        if ":" not in line:
            raise ValueError(f"unsupported frontmatter line: {line!r}")
        key, value = line.split(":", 1)
        fields[key.strip()] = value.strip().strip('"').strip("'")
    return fields


class RepoSkillTests(unittest.TestCase):
    def test_expected_skills_are_well_formed(self) -> None:
        self.assertTrue(SKILLS_ROOT.is_dir())
        for skill_name in sorted(EXPECTED_SKILLS):
            with self.subTest(skill=skill_name):
                self.assertRegex(skill_name, NAME_RE)
                skill_dir = SKILLS_ROOT / skill_name
                skill_path = skill_dir / "SKILL.md"
                metadata_path = skill_dir / "agents" / "openai.yaml"
                self.assertTrue(skill_path.is_file())
                self.assertTrue(metadata_path.is_file())

                text = skill_path.read_text(encoding="utf-8")
                fields = parse_frontmatter(text)
                self.assertEqual(set(fields), {"name", "description"})
                self.assertEqual(fields["name"], skill_name)
                self.assertTrue(fields["description"])
                self.assertNotIn("TODO", text)

                metadata = metadata_path.read_text(encoding="utf-8")
                for key in (
                    "display_name:",
                    "short_description:",
                    "default_prompt:",
                ):
                    self.assertIn(key, metadata)
                self.assertIn(f"${skill_name}", metadata)
                self.assertNotIn("TODO", metadata)


if __name__ == "__main__":
    unittest.main()

# PicoSwitch2

@AGENTS.md

## Claude Code specifics

- The line above imports `AGENTS.md`, which is the single source of project
  rules for every agent. Do not copy rules into this file.
- How work is done here (evidence standards, experiments, documentation,
  completion) is in `docs/engineering-principles.md`. Read it before
  reverse-engineering work or before writing protocol documentation.
- Project skills are in `.claude/skills/`. Prefer the matching skill over
  improvising a procedure; the list is in `AGENTS.md`.
- Local machine facts (tool paths, test rigs) belong in Claude's memory, not in
  the repository.

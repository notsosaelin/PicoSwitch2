#!/usr/bin/env python3
"""Verify every management command the companion can send is reachable over BLE.

WHY THIS EXISTS
---------------
A command reaches the firmware's dispatcher (`handle_line()` in src/config.c) and,
independently, must be accepted by the wireless allowlist
(`config_wireless_command_allowed()` in src/config_wireless_bridge.c). Those are two
separate lists, and only the second one gates the BLE transport.

A verb added to the dispatcher but not the allowlist compiles, passes every unit test,
and works perfectly over USB CDC and UART -- while answering `unknown command` over BLE.
The companion probes capabilities by looking for exactly that reply, so it reports the
feature as missing and tells the user to update firmware that plainly already has it.

That is not hypothetical: `pairing start` / `pairing status` / `pairing cancel` shipped
that way. Nothing failed, because nothing compared the two sides.

WHAT IS CHECKED
---------------
  1. Every command ManagementCommands (Kotlin) can emit is accepted by the C allowlist.
  2. Every one of them is also reachable in the C dispatcher.
  3. Every strncmp() prefix length in both C files equals the literal's own length --
     an off-by-one there silently widens or narrows the surface.
  4. Nothing in the Kotlin command object is left unclassified. Anything this parser
     cannot resolve is a FAILURE, never a silent skip.

It also prints the authoritative command matrix, including firmware verbs the companion
does not use (informational -- UART/lab tooling is deliberately not on the allowlist).

Usage: python tools/check_management_command_parity.py [--quiet]
Exit code 0 when the surfaces agree, 1 otherwise.
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
KOTLIN = (ROOT / "android" / "companion" / "management-core" / "src" / "main" / "kotlin" /
          "dev" / "picoswitch" / "management" / "ManagementProtocol.kt")
DOMAIN = (ROOT / "android" / "companion" / "management-core" / "src" / "main" / "kotlin" /
          "dev" / "picoswitch" / "management" / "Domain.kt")
ALLOWLIST_C = ROOT / "src" / "config_wireless_bridge.c"
DISPATCH_C = ROOT / "src" / "config.c"

# Commands whose verb is not a literal in ManagementCommands.
#
# ManagementCommands.color() builds "<ColorTarget.command> <rgb>", so the verb comes from an
# enum. The expected set is pinned here and verified against Domain.kt below, which means
# adding a colour target fails this check until the firmware side is considered too.
DYNAMIC_VERBS = {
    "ColorTarget": {"body", "jcl", "jcr"},
}


def _strip_block_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def _strip_line_comments(text: str) -> str:
    """Drop // comments without touching // inside string literals."""
    out = []
    in_string = False
    escape = False
    index = 0
    while index < len(text):
        char = text[index]
        if in_string:
            out.append(char)
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == '"':
                in_string = False
            index += 1
            continue
        if char == '"':
            in_string = True
            out.append(char)
            index += 1
            continue
        if char == "/" and index + 1 < len(text) and text[index + 1] == "/":
            while index < len(text) and text[index] != "\n":
                index += 1
            continue
        out.append(char)
        index += 1
    return "".join(out)


def _extract_block(text: str, start_pattern: str) -> str:
    """Return the text between the first '{' after start_pattern and its matching '}'."""
    match = re.search(start_pattern, text, flags=re.M)
    if not match:
        raise SystemExit(f"could not locate {start_pattern!r}")
    open_index = text.index("{", match.end())
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:index]
    raise SystemExit(f"unterminated block for {start_pattern!r}")


def _strip_call(text: str, name: str) -> str:
    """Remove `name(...)` calls, plus any trailing `{ ... }` lambda, with nesting."""
    out = []
    index = 0
    while True:
        found = text.find(name + "(", index)
        if found < 0:
            out.append(text[index:])
            return "".join(out)
        out.append(text[index:found])
        cursor = found + len(name)
        depth = 0
        while cursor < len(text):
            if text[cursor] == "(":
                depth += 1
            elif text[cursor] == ")":
                depth -= 1
                if depth == 0:
                    cursor += 1
                    break
            cursor += 1
        # An attached trailing lambda carries the failure message; drop it too.
        probe = cursor
        while probe < len(text) and text[probe] in " \t":
            probe += 1
        if probe < len(text) and text[probe] == "{":
            depth = 0
            while probe < len(text):
                if text[probe] == "{":
                    depth += 1
                elif text[probe] == "}":
                    depth -= 1
                    if depth == 0:
                        probe += 1
                        break
                probe += 1
            cursor = probe
        index = cursor


def kotlin_commands() -> tuple[list[tuple[str, bool]], list[str]]:
    """Return ([(static_prefix, is_prefix)], [unclassified]) from ManagementCommands."""
    text = _strip_line_comments(_strip_block_comments(KOTLIN.read_text(encoding="utf-8")))
    block = _extract_block(text, r"^object ManagementCommands")
    block = _strip_call(block, "require")
    block = _strip_call(block, "Regex")

    commands: list[tuple[str, bool]] = []
    unclassified: list[str] = []

    # Scan literals. A command's STATIC PREFIX is the text before its first interpolation;
    # everything from `$` onward is a runtime value. Interpolations are skipped wholesale,
    # including any string literals inside them, so a value interpolated INTO a command
    # (the "default" in "${destination?.wire ?: "default"}", the "none" in
    # "${if (sourceId == 0L) "none" else sourceId}") is never mistaken for a command.
    index = 0
    while index < len(block):
        if block[index] != '"':
            index += 1
            continue

        index += 1
        literal = []
        truncated = False
        brace_depth = 0     # ${ } nesting within this literal
        while index < len(block):
            char = block[index]
            if char == "\\":
                if brace_depth == 0 and not truncated:
                    literal.append(block[index:index + 2])
                index += 2
                continue
            if brace_depth > 0:
                if char == "{":
                    brace_depth += 1
                elif char == "}":
                    brace_depth -= 1
                elif char == '"':
                    index += 1
                    while index < len(block):
                        if block[index] == "\\":
                            index += 2
                            continue
                        if block[index] == '"':
                            break
                        index += 1
                index += 1
                continue
            if char == '"':
                index += 1
                break
            if char == "$":
                truncated = True
                if block[index + 1:index + 2] == "{":
                    brace_depth = 1
                    index += 2
                else:
                    index += 1
                    while index < len(block) and (block[index].isalnum() or block[index] == "_"):
                        index += 1
                continue
            if not truncated:
                literal.append(char)
            index += 1

        value = "".join(literal)
        if not value or not re.match(r"^[a-z]", value):
            continue        # format specifiers, regexes and human-readable messages
        commands.append((value, truncated))

    # A command that begins with an interpolation has an empty static prefix and cannot be
    # classified from the literal alone; those must be covered by DYNAMIC_VERBS.
    for verb_source, verbs in DYNAMIC_VERBS.items():
        actual = _enum_wire_values(verb_source)
        if actual != verbs:
            unclassified.append(
                f"{verb_source} in Domain.kt is {sorted(actual)}, "
                f"but this checker pins {sorted(verbs)} -- update both sides deliberately")
        for verb in sorted(verbs):
            commands.append((verb + " ", True))

    return commands, unclassified


def _enum_wire_values(name: str) -> set[str]:
    text = _strip_line_comments(_strip_block_comments(DOMAIN.read_text(encoding="utf-8")))
    block = _extract_block(text, rf"enum class {name}\s*\(")
    return set(re.findall(r'^\s*\w+\("([^"]+)"\)', block, flags=re.M))


def c_surface(path: pathlib.Path, function_pattern: str) -> tuple[set[str], set[str], list[str]]:
    """Return (exact commands, prefixes, length errors) for one C matcher function."""
    text = _strip_line_comments(_strip_block_comments(path.read_text(encoding="utf-8")))
    block = _extract_block(text, function_pattern)

    exact = set(re.findall(r'strcmp\(\s*\w+\s*,\s*"([^"]*)"\s*\)\s*==\s*0', block))
    prefixes: set[str] = set()
    errors: list[str] = []
    for literal, length in re.findall(
            r'strncmp\(\s*\w+\s*,\s*"([^"]*)"\s*,\s*(\d+)\w*\s*\)\s*==\s*0', block):
        prefixes.add(literal)
        if len(literal) != int(length):
            errors.append(
                f"{path.name}: strncmp(\"{literal}\", {length}) -- literal is "
                f"{len(literal)} bytes, so the compared span is wrong")
    return exact, prefixes, errors


def accepted(command: str, is_prefix: bool, exact: set[str], prefixes: set[str]) -> bool:
    if not is_prefix and command in exact:
        return True
    for prefix in prefixes:
        if command.startswith(prefix):
            return True
        # The Kotlin side's static text is shorter than the C prefix, so a match cannot be
        # decided from source alone. Treat as not accepted; it needs an explicit rule.
    return False


def main() -> int:
    quiet = "--quiet" in sys.argv
    failures: list[str] = []

    commands, unclassified = kotlin_commands()
    failures.extend(unclassified)

    allow_exact, allow_prefixes, allow_errors = c_surface(
        ALLOWLIST_C, r"bool config_wireless_command_allowed")
    dispatch_exact, dispatch_prefixes, dispatch_errors = c_surface(
        DISPATCH_C, r"static void handle_line")
    failures.extend(allow_errors)
    failures.extend(dispatch_errors)

    rows = []
    for command, is_prefix in sorted(set(commands)):
        ble = accepted(command, is_prefix, allow_exact, allow_prefixes)
        handled = accepted(command, is_prefix, dispatch_exact, dispatch_prefixes)
        rows.append((command, is_prefix, ble, handled))
        shown = command + ("..." if is_prefix else "")
        if not handled:
            failures.append(
                f"companion sends {shown!r} but handle_line() in src/config.c does not "
                f"dispatch it")
        elif not ble:
            failures.append(
                f"companion sends {shown!r}, handle_line() dispatches it, but "
                f"config_wireless_command_allowed() rejects it -- works over USB CDC and "
                f"UART, answers `unknown command` over BLE. This is the `pairing` bug.")

    if not quiet:
        print(f"{'command':<24} {'form':<7} {'BLE':<5} dispatcher")
        print("-" * 52)
        for command, is_prefix, ble, handled in rows:
            print(f"{command:<24} {'prefix' if is_prefix else 'exact':<7} "
                  f"{'yes' if ble else 'NO':<5} {'yes' if handled else 'NO'}")

        firmware_only = sorted(
            (dispatch_exact | dispatch_prefixes) -
            {c for c, _ in commands} -
            {c.rstrip() for c, _ in commands})
        unused = [v for v in firmware_only
                  if not any(v.startswith(c) or c.startswith(v) for c, _ in commands)]
        print(f"\nfirmware verbs the companion does not use ({len(unused)}):")
        print("  " + ", ".join(repr(v) for v in unused))
        print("  (UART/lab diagnostics are deliberately off the BLE allowlist)")

    if failures:
        print("\nmanagement command parity FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(f"\nmanagement command parity OK: {len(rows)} companion commands, "
          f"all dispatched and all reachable over BLE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

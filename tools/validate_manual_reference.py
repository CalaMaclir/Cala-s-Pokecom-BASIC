#!/usr/bin/env python3
"""Validate the current reference inventory, compatibility markers and examples."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "docs/programming-reference-ja.md"
COMPILER = ROOT / "src/core/basic_compiler.cpp"
REPL = ROOT / "src/core/repl.cpp"

STATEMENTS = {
    "Assignment / LET": ("LET",),
    "PRINT": ("PRINT",),
    "INPUT": ("INPUT",),
    "DIM": ("DIM",),
    "REM / apostrophe comment": ("REM",),
    "Single-line IF": ("IF", "THEN"),
    "Block IF / ELSEIF / ELSE / END IF": ("IF", "ELSEIF", "ELSE"),
    "FOR / NEXT": ("FOR", "NEXT"),
    "WHILE / WEND": ("WHILE", "WEND"),
    "DO / LOOP": ("DO", "LOOP"),
    "EXIT FOR": ("EXIT", "FOR"),
    "EXIT DO": ("EXIT", "DO"),
    "GOTO": ("GOTO",),
    "GOSUB": ("GOSUB",),
    "RETURN - Classic GOSUB return": ("RETURN",),
    "ON ... GOTO": ("ON", "GOTO"),
    "ON ... GOSUB": ("ON", "GOSUB"),
    "SELECT CASE": ("SELECT", "CASE"),
    "CASE": ("CASE",),
    "CASE ELSE": ("CASE", "ELSE"),
    "END SELECT": ("END", "SELECT"),
    "FUNCTION": ("FUNCTION",),
    "END FUNCTION": ("FUNCTION",),
    "GLOBAL": ("GLOBAL",),
    "RETURN expression - Structured FUNCTION return": ("RETURN",),
    "DATA": ("DATA",),
    "READ": ("READ",),
    "RESTORE": ("RESTORE",),
    "END": ("END",),
    "STOP": ("STOP",),
    "SLEEP": ("SLEEP",),
    "PAUSE": ("PAUSE",),
    "RANDOMIZE": ("RANDOMIZE",),
    "LOCATE": ("LOCATE",),
    "SCREEN": ("SCREEN",),
    "CLS statement": ("CLS",),
    "COLOR": ("COLOR",),
    "COLORHSV": ("COLORHSV",),
    "PSET": ("PSET",),
    "LINE": ("LINE",),
    "CIRCLE": ("CIRCLE",),
    "BOX": ("BOX",),
    "PAINT": ("PAINT",),
    "FLUSH": ("FLUSH",),
    "GLOCATE": ("GLOCATE",),
    "GPRINT": ("GPRINT",),
    "GDEF": ("GDEF",),
    "GPALETTE": ("GPALETTE",),
    "LOADIMAGE": ("LOADIMAGE",),
    "SAVEIMAGE / SAVE IMAGE": ("SAVEIMAGE", "SAVE"),
    "BEEP": ("BEEP",),
    "PLAY": ("PLAY",),
    "WAVPLAY": ("WAVPLAY",),
    "WAVPAUSE": ("WAVPAUSE",),
    "WAVRESUME": ("WAVRESUME",),
    "WAVSTOP": ("WAVSTOP",),
    "I2C SCAN": ("I2C", "SCAN"),
    "I2CWRITE": ("I2CWRITE",),
    r"Operators \\, <<, >>, XOR": (),
}

FUNCTIONS = (
    "ABS", "ACOS", "ASC", "ASIN", "ATAN2", "ATN", "CHR$", "CLAMP",
    "COS", "DEG", "EXP", "I2CREAD", "INKEY", "INSTR", "INT", "LEFT$",
    "LEN", "LN", "LOG", "MAX", "MID$", "MIN", "PI", "PLAYING", "POINT",
    "RAD", "RND", "RNDI", "RIGHT$", "SGN", "SIN", "SPC", "SQR", "STR$",
    "STRING$", "TAB", "TAN", "TIMER", "VAL",
    "TRIM$", "LTRIM$", "RTRIM$", "REPLACE$", "SPACE$", "DATE$", "TIME$",
)

REPL_ENTRIES = {
    "EDIT": "EDIT", "LIST": "LIST", "RUN": "RUN", "NEW": "NEW",
    "CLEAR": "CLEAR", "CLS command": "CLS", "LOAD": "LOAD",
    "SAVE command": "SAVE", "FILES": "FILES", "DIR": "DIR", "SD": "SD",
    "SCREENSHOT": "SCREENSHOT", "XRECV": "XRECV", "XSEND": "XSEND",
    "YRECV": "YRECV", "YSEND": "YSEND", "DATE": "DATE", "TIME": "TIME",
    "DATETIME": "DATETIME", "SERIAL": "SERIAL", "CONSOLE": "CONSOLE",
    "DIAGNOSTICS": "DIAGNOSTICS", "INFO": "INFO", "LASTERROR": "LASTERROR",
    "PROFILE": "PROFILE", "STANDBY": "STANDBY", "MENU": "MENU", "HELP": "HELP",
}

MARKER = re.compile(
    r"> \*\*対応モード:\*\* Classic=(対応|非対応|共通|利用可) / "
    r"Structured=(対応|非対応|共通|利用可)"
)


def sections(markdown: str) -> dict[str, list[str]]:
    matches = list(re.finditer(r"^### (.+)$", markdown, re.M))
    result: dict[str, list[str]] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(markdown)
        result.setdefault(match.group(1), []).append(markdown[match.end():end])
    return result


def require_entry(blocks: dict[str, list[str]], name: str) -> None:
    assert name in blocks, f"missing entry heading: {name}"
    valid = []
    for block in blocks[name]:
        markers = list(MARKER.finditer(block))
        if len(markers) == 1 and not block[:markers[0].start()].strip() and "```" in block:
            valid.append(block)
    assert len(valid) == 1, f"entry requires one adjacent compatibility marker and example: {name}"



def main() -> None:
    markdown = REFERENCE.read_text(encoding="utf-8")
    compiler = COMPILER.read_text(encoding="utf-8")
    repl = REPL.read_text(encoding="utf-8")
    blocks = sections(markdown)

    entries = list(STATEMENTS) + list(FUNCTIONS) + list(REPL_ENTRIES)
    assert len(entries) == len(set(entries)), "duplicate canonical inventory entry"
    assert len(MARKER.findall(markdown)) == len(entries), "orphan or duplicate compatibility metadata"
    for name in entries:
        require_entry(blocks, name)

    # Function names are extracted from the compiler, including the four optimized
    # v0.93 names whose comparisons are deliberately written without strcmp.
    implementation_functions = set(re.findall(r'std::strcmp\(name, "([A-Z0-9$]+)"\)', compiler))
    for name in ("LN", "ASIN", "ACOS", "ATAN2"):
        if f'FnId::{name}' in compiler:
            implementation_functions.add(name)
    assert implementation_functions == set(FUNCTIONS), (
        sorted(implementation_functions - set(FUNCTIONS)),
        sorted(set(FUNCTIONS) - implementation_functions),
    )

    # Every canonical statement token must still be present in the compiler.
    for entry, tokens in STATEMENTS.items():
        for token in tokens:
            assert f'"{token}"' in compiler, (entry, token)

    # REPL inventory is compared with the concrete command recognizers.
    recognized = set(re.findall(r'command_(?:equals|argument)\(input,\s*"([A-Z]+)"', repl))
    recognized.update(re.findall(r'\{"(FILES)",\s*"(DIR)"\}', repl)[0])
    assert recognized == set(REPL_ENTRIES.values()), (
        sorted(recognized - set(REPL_ENTRIES.values())),
        sorted(set(REPL_ENTRIES.values()) - recognized),
    )

    quick = markdown.split("## 23. Quick Reference（alphabetical）", 1)[1]
    for name in entries:
        if name.startswith("Operators "):
            continue
        assert f"`{name}`" in quick, f"missing from final Quick Reference: {name}"

    assert "Classic=対応 / Structured=非対応" in markdown
    assert "Classic=非対応 / Structured=対応" in markdown
    assert "Classic=対応 / Structured=対応" in markdown
    assert "Classic=共通 / Structured=共通" in markdown
    print(
        "v0.94 manual reference validation: PASS - "
        f"{len(entries)} entries, {len(entries)} compatibility markers, "
        f"{len(entries)} example sections"
    )


if __name__ == "__main__":
    main()

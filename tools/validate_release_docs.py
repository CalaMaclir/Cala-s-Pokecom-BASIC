"""Validate current v0.94 manuals and immutable v0.92 historical release evidence."""

from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HISTORICAL = {
    "docs/Cala-Pokecom-BASIC-v0.92-Install-Manual-ja.pdf": "64b033298f28b637b2012b5f9d72db77f9aeeba41953dda266d6b25bd00b34ce",
    "docs/Cala-Pokecom-BASIC-v0.92-System-Manual-ja.pdf": "e63b2ee41f7c0540049105066eafca60244205fa8f43d8c3d740b7d9a730c56f",
    "docs/Cala-Pokecom-BASIC-v0.92-Programming-Reference-ja.pdf": "b5e93747bbecb7d67d6a5dbb0dc723743d566ac57bfb551507efa7b02ce8d516",
    "docs/archive/v0.92/programming-reference-ja.md": "a8d00cff8f82c2e392101a22f07a7e62e1ffe35f288f1f6bfc3180c647529a0f",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    firmware = re.search(
        r'set\(RMB_VERSION "([^"\n]+)"',
        (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
    ).group(1)
    assert firmware == "0.94", firmware

    il = (ROOT / "include/il.hpp").read_text(encoding="utf-8")
    for name, value in {
        "kMaxUserFunctions": 32, "kMaxFunctionLocals": 128,
        "kMaxFunctionParameters": 8, "kMaxFunctionCallDepth": 16,
        "kMaxOps": 1536, "kStringPoolSize": 6144, "kMaxSymbols": 64,
    }.items():
        assert re.search(r"\b" + name + r"\s*=\s*" + str(value) + r"\s*;", il), name

    for filename, expected in HISTORICAL.items():
        assert digest(ROOT / filename) == expected, (filename, "historical artifact changed")
    for archived in ("install-manual-ja.md", "system-manual-ja.md", "programming-reference-ja.md"):
        source = ROOT / "docs/archive/v0.92" / archived
        assert source.is_file() and "## Version 0.92" in source.read_text(encoding="utf-8"), source

    manifest = json.loads((ROOT / "docs/manuals-manifest.json").read_text(encoding="utf-8"))
    assert manifest["version"] == firmware and len(manifest["manuals"]) == 3
    assert digest(ROOT / manifest["generator"]) == manifest["generator_sha256"]
    for manual in manifest["manuals"]:
        source, output = ROOT / manual["source"], ROOT / manual["output"]
        assert f"## Version {firmware}" in source.read_text(encoding="utf-8")
        assert digest(source) == manual["source_sha256"]
        assert digest(output) == manual["output_sha256"]
        assert output.read_bytes().startswith(b"%PDF-")

    index = (ROOT / "docs/manual-ja.md").read_text(encoding="utf-8")
    assert index.startswith(f"# Cala's Pokecom BASIC Version {firmware} 日本語マニュアル\n")
    for target in re.findall(r"\]\(([^)]+)\)", index):
        assert (ROOT / "docs" / target).is_file(), ("manual index link", target)
    for readme in ("README.md", "README.en.md"):
        text = (ROOT / readme).read_text(encoding="utf-8")
        assert f"Version {firmware}" in text and "docs/manual-ja.md" in text
        for manual in manifest["manuals"]:
            assert f'({manual["output"]})' in text, (readme, manual["output"])

    install = (ROOT / "docs/install-manual-ja.md").read_text(encoding="utf-8")
    for phrase in (
        "ZIP全体を展開", "flash-cpb.cmd", "build/CPokecombasic.uf2",
        "Keyboard BIOS更新は通常利用の必須条件ではありません",
    ):
        assert phrase in install, phrase

    system = (ROOT / "docs/system-manual-ja.md").read_text(encoding="utf-8")
    for phrase in (
        "Alt+E", "Alt+R", "Alt+C", "Alt+M", "Outdent", "System Information",
        "Keyboard I2C", "STANDBY", "USB Storage", "Program Storage", "最大5件",
        "5秒間観測", "そのまま表示", "strongest Enabled AP", "起動ごとにOFF",
    ):
        assert phrase in system, phrase

    reference = (ROOT / "docs/programming-reference-ja.md").read_text(encoding="utf-8")
    for phrase in (
        "LOG(x) changed from natural logarithm to base-10 logarithm",
        "真のINTEGER型や`A%`はありません", "RETURN - Classic GOSUB return",
        "RETURN expression - Structured FUNCTION return", "Direct modeは非対応",
    ):
        assert phrase in reference, phrase

    for required in (
        "docs/release/v0.94-release-notes.md",
        "docs/release/v0.94-known-limitations.md",
        "docs/release/v0.94-release-checklist.md",
    ):
        assert (ROOT / required).is_file(), required

    assert "format 6" in system and "format 3" not in system
    for phrase in ("LOADIMAGE", "SAVEIMAGE", "SAVE IMAGE", "INFO", "LASTERROR", "auto"):
        assert phrase in reference or phrase in system, phrase
    subprocess.run([sys.executable, str(ROOT / "tools/validate_manual_reference.py")], check=True)
    print("v0.94 release documentation and immutable v0.92 evidence: PASS")



if __name__ == "__main__":
    main()

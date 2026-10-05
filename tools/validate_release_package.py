#!/usr/bin/env python3
"""Verify a staged Actions package or its original downloaded ZIP, without flashing."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import zipfile


def validate(files: dict[str, bytes]) -> dict:
    assert files, "empty package"
    for name in files:
        path = PurePosixPath(name)
        assert not path.is_absolute() and ".." not in path.parts and "\\" not in name, name
        assert not any(part.startswith(".") for part in path.parts), name
        assert path.suffix.lower() not in {".zip", ".elf", ".map", ".o", ".obj", ".tmp"}, name
        if path.suffix.lower() == ".uf2":
            assert name == "build/CPokecombasic.uf2", ("non-production firmware", name)
    required = {"build/CPokecombasic.uf2", "flash-cpb.cmd", "README.md", "README.en.md",
                "build/firmware-manifest.json", "build/package-audit.json", "docs/manuals-manifest.json",
                "docs/release/v0.94-release-notes.md", "docs/release/v0.94-release-checklist.md",
                "docs/release/v0.94-known-limitations.md"}
    assert required <= files.keys(), sorted(required - files.keys())
    audit = json.loads(files["build/package-audit.json"])
    entries = audit["files"]
    names = [item["path"] for item in entries]
    assert len(names) == len(set(names)), "duplicate audit entries"
    assert set(names) == files.keys() - {"build/package-audit.json"}, "untracked package file"
    for item in entries:
        assert hashlib.sha256(files[item["path"]]).hexdigest() == item["sha256"], item["path"]
    firmware = json.loads(files["build/firmware-manifest.json"])
    assert firmware["firmware_version"] == "0.94"
    assert audit["source_sha"] == firmware["checkout_sha"]
    assert firmware["uf2_sha256"] == hashlib.sha256(files["build/CPokecombasic.uf2"]).hexdigest()
    assert len(files["build/CPokecombasic.uf2"]) == firmware["uf2_bytes"] > 0
    assert firmware["host_results"]["tests"] >= 70
    assert all(firmware["host_results"][key] == 0 for key in ("failures", "errors", "skipped"))
    updater = files["flash-cpb.cmd"].decode("utf-8").lower()
    assert "build\\cpokecombasic.uf2" in updater, "updater/firmware layout mismatch"
    manuals = json.loads(files["docs/manuals-manifest.json"])
    assert manuals == firmware["manuals"] and manuals["version"] == "0.94"
    for manual in manuals["manuals"]:
        for key in ("source", "output"):
            assert hashlib.sha256(files[manual[key]]).hexdigest() == manual[key + "_sha256"]
        assert files[manual["output"]].startswith(b"%PDF-")
    return {"pass": True, "source_sha": firmware["checkout_sha"], "files": len(files),
            "tests": firmware["host_results"]["tests"], "build": firmware["build_number"],
            "hardware_acceptance": firmware["hardware_acceptance"]}


def read_package(path: Path) -> dict[str, bytes]:
    if path.is_dir():
        return {str(p.relative_to(path)): p.read_bytes() for p in path.rglob("*") if p.is_file()}
    with zipfile.ZipFile(path) as archive:
        names = [item.filename for item in archive.infolist() if not item.is_dir()]
        assert len(names) == len(set(names)), "duplicate ZIP entries"
        return {name: archive.read(name) for name in names}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(read_package(args.package)), indent=2))


if __name__ == "__main__":
    main()

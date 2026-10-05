#!/usr/bin/env python3
"""Collect native soak evidence without claiming device heap or hardware acceptance."""
import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

PATTERN = re.compile(r"SOAK scope=(\S+) cycle=(\d+) handles=(-?\d+) baseline_handles=(-?\d+) host_heap=(-?\d+) baseline_heap=(-?\d+)")
REQUIRED = {"image", "image-native", "image-statements", "program-protection", "classic-structured",
            "error-editor-report", "diagnostic-serial", "files-audio-esc"}


def collect(path):
    cases = list(ET.parse(path).getroot().iter("testcase"))
    assert cases and all(case.find(key) is None for case in cases for key in ("failure", "error", "skipped"))
    scopes = {}
    for case in cases:
        output = "".join(case.itertext())
        for match in PATTERN.finditer(output):
            scope, cycle, handles, baseline_handles, heap, baseline_heap = match.groups()
            sample = dict(zip(("cycle", "handles", "baseline_handles", "host_heap", "baseline_heap"),
                              map(int, (cycle, handles, baseline_handles, heap, baseline_heap))))
            assert sample["handles"] == sample["baseline_handles"]
            if sample["host_heap"] >= 0:
                assert sample["host_heap"] <= sample["baseline_heap"] + 65536
            else:
                sample["host_heap"] = sample["baseline_heap"] = None
            scopes.setdefault(scope, []).append(sample)
    assert REQUIRED <= scopes.keys(), sorted(REQUIRED - scopes.keys())
    for scope, samples in scopes.items():
        assert samples[-1]["cycle"] >= (50 if scope == "program-protection" else 200), scope
    return {"host_stability_pass": True, "hardware_status": "NOT_RUN", "scopes": scopes,
            "device_memory": "NOT_RUN", "native_heap_allowance_bytes": 65536,
            "program_safety_scenarios": 50 * 3 * 2,
            "audio_pwm_cycles": 200,
            "limits": "Host codec/files are real; LCD/SD driver/radio/USB/electrical power use fixtures. ASan heap values are N/A. RP2350 heap/fragmentation, noise, power and long-run acceptance require device reports."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("build-host/test-results.xml"))
    parser.add_argument("--output", type=Path, default=Path("build/stability-evidence.json"))
    args = parser.parse_args()
    result = collect(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("Native stability evidence: PASS; hardware / device memory: NOT_RUN")


if __name__ == "__main__":
    main()

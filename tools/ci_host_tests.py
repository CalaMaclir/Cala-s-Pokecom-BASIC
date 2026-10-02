#!/usr/bin/env python3
"""Build cached native regression tests in parallel; always execute them serially."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
# Migration contract: do not accidentally drop an existing test while optimizing CI.
REQUIRED_TESTS = set("""
status-layout-test system-controls-test display-scroll-test prompt-boundary-test
xmodem-test ymodem-test file-transfer-menu-test menu-scroll-test command-history-test
file-management-test editor-model-test psram-layout-test psram-allocator-test
compiled-cache-test psram-editor-history-test usb-storage-menu-test http-core-test
safe-file-test serial-route-test graphics-text-test audio-engine-test rtc-codec-test
key-click-test bluetooth-hid-keyboard-core-test bluetooth-device-registry-test
removed-bluetooth-transport ble-security-recovery bluetooth-registry-storage-test
runtime-test program-storage-test program-session-test ble-report-sdk-parser
usb-msc-test storage-ownership-test
""".split())
STAGE1_REQUIRED_TESTS = {"editor-structure-test", "file-path-test", "program-pair-test", "stage1-source-contract"}
STAGE23_REQUIRED_TESTS = {"structured-source-test", "path-compaction-test", "classic-benchmark",
                         "files-ui-integration-test", "performance-benchmark", "stage3-optimizer-test", "paired-benchmark-protocol",
                         "benchmark-semantic-contract", "performance-policy-regression"}
REMOVED_TRANSPORT = re.compile(rb"RFCOMM|BLUETOOTH_SERIAL|Serial Port Profile|SPP Test Terminal|Bluetooth Console")


def parallel_budget(cpus: int, available_bytes: int, requested: int) -> int:
    if requested < 1:
        raise ValueError("CPB_HOST_JOBS must be a positive integer")
    # Conservative initial budget: at most four compiler processes by default,
    # reserving 1.5 GiB of available RAM per process. Runtime tests stay serial.
    memory_slots = max(1, available_bytes // (1536 * 1024 * 1024))
    return max(1, min(max(1, cpus), memory_slots, requested))


def available_memory() -> int:
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    return 1536 * 1024 * 1024


def cache_stats(env: dict[str, str]) -> dict[str, int]:
    result = subprocess.run(["ccache", "--print-stats"], env=env, text=True,
                            stdout=subprocess.PIPE, check=True)
    stats: dict[str, int] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2:
            try:
                stats[fields[0]] = int(fields[1])
            except ValueError:
                pass
    return stats


def cache_summary(before: dict[str, int], after: dict[str, int]) -> dict:
    # Canonical keys from ccache --print-stats (not human-readable labels).
    required = ("direct_cache_hit", "preprocessed_cache_hit", "cache_miss")
    if any(key not in before or key not in after for key in required):
        raise ValueError("Missing machine-readable ccache counters")
    keys = required + ("called_for_link", "multiple_source_files")
    delta = {key: after.get(key, 0) - before.get(key, 0) for key in keys}
    if any(value < 0 for value in delta.values()):
        raise ValueError("Host cache counters reset during this run")
    hits = delta["direct_cache_hit"] + delta["preprocessed_cache_hit"]
    calls = hits + delta["cache_miss"]
    return {"cache_delta": delta, "cacheable_calls": calls,
            "cache_hit_percent": round(100 * hits / calls, 2) if calls else None}


def check_removed_transport() -> None:
    found = False
    for directory in (ROOT / "include", ROOT / "src"):
        if not directory.is_dir():
            raise FileNotFoundError(directory)
        for path in sorted(directory.rglob("*")):
            if not path.is_file():
                continue
            for number, line in enumerate(path.read_bytes().splitlines(), 1):
                if REMOVED_TRANSPORT.search(line):
                    print(f"{path.relative_to(ROOT)}:{number}: {line.decode('utf-8', errors='replace')}")
                    found = True
    if found:
        raise RuntimeError("Removed Bluetooth transport symbol remains in current source")
    print("Removed Bluetooth transport source guard: PASS")


def audit_build(build: Path) -> None:
    commands = json.loads((build / "compile_commands.json").read_text())
    if not commands:
        raise RuntimeError("Empty host compilation database")
    for entry in commands:
        args = entry.get("arguments") or shlex.split(entry["command"])
        if "-UNDEBUG" not in args or not any("assertions_enabled.hpp" in value for value in args):
            raise RuntimeError(f"Missing assertion guard: {entry['file']}")
        if "-c" not in args:
            raise RuntimeError("Host compilation must be one source per object")
    print(f"Assertion guards and individual compilation: {len(commands)} objects checked", flush=True)


def run() -> int:
    for executable in ("cmake", "ninja", "g++", "ccache", "ctest"):
        if not shutil.which(executable):
            raise RuntimeError(f"Missing host build dependency: {executable}")
    build = ROOT / "build-host"
    build.mkdir(exist_ok=True)
    env = dict(os.environ)
    # Separate from the firmware cache: never reset or evict its existing entries.
    env["CCACHE_DIR"] = str(Path.home() / ".cache" / "cpb-host-ccache")
    env["CCACHE_MAXSIZE"] = "1G"
    env["CCACHE_BASEDIR"] = str(ROOT)
    cpus = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 1)
    memory = available_memory()
    jobs = parallel_budget(cpus, memory, int(env.get("CPB_HOST_JOBS", "4")))
    metrics: dict = {
        "status": "running", "run_number": env.get("GITHUB_RUN_NUMBER", "local"),
        "run_attempt": env.get("GITHUB_RUN_ATTEMPT", "local"),
        "commit": env.get("GITHUB_SHA", "local"),
        "runner": env.get("RUNNER_NAME", "local"),
        "compiler_jobs": jobs, "test_jobs": 1, "available_cpus": cpus,
        "available_memory_bytes": memory, "cache_directory": env["CCACHE_DIR"],
        "phases_seconds": {},
    }
    before = cache_stats(env)
    start = time.monotonic()
    print(f"Host build: {jobs} compiler jobs, serial tests; available CPUs={cpus}, RAM={memory // (1024**2)} MiB", flush=True)

    def phase(name: str, command: list[str]) -> None:
        print(f"[{name}] {shlex.join(command)}", flush=True)
        tick = time.monotonic()
        try:
            subprocess.run(command, cwd=ROOT, env=env, check=True)
        finally:
            metrics["phases_seconds"][name] = round(time.monotonic() - tick, 3)

    code = 0
    try:
        phase("configure", ["cmake", "-S", "tests/host", "-B", str(build), "-G", "Ninja",
                            f"-DCMAKE_CXX_COMPILER={shutil.which('g++')}", "-DCMAKE_BUILD_TYPE="])
        audit_build(build)
        phase("compile_and_link", ["cmake", "--build", str(build), "--parallel", str(jobs)])
        inventory = json.loads(subprocess.run(
            ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
            env=env, text=True, stdout=subprocess.PIPE, check=True).stdout)
        tests = inventory["tests"]
        names = {test["name"] for test in tests}
        if not (REQUIRED_TESTS | STAGE1_REQUIRED_TESTS | STAGE23_REQUIRED_TESTS).issubset(names) or len(names) != len(tests):
            raise RuntimeError(f"Missing/duplicate host tests: {sorted(REQUIRED_TESTS - names)}")
        for test in tests:
            properties = {p["name"]: p["value"] for p in test.get("properties", [])}
            if properties.get("DISABLED") or not properties.get("RUN_SERIAL"):
                raise RuntimeError(f"Disabled or non-serial host test: {test['name']}")
        metrics["registered_tests"] = len(tests)
        (build / "test-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        phase("test_execution", ["ctest", "--test-dir", str(build), "--parallel", "1",
                                 "--output-on-failure", "--no-tests=error",
                                 "--output-junit", str(build / "test-results.xml")])
        cases = ET.parse(build / "test-results.xml").getroot().findall(".//testcase")
        if len(cases) != len(tests) or any(case.find("skipped") is not None for case in cases):
            raise RuntimeError("Test result count mismatch or skipped regression test")
        metrics["executed_tests"] = len(cases)
        metrics["status"] = "success"
    except (subprocess.CalledProcessError, RuntimeError, OSError, ValueError) as error:
        code = error.returncode if isinstance(error, subprocess.CalledProcessError) else 1
        metrics["status"] = "failure"
        metrics["error"] = str(error)
        print(f"Host validation failed: {error}", file=sys.stderr, flush=True)
    finally:
        metrics["total_seconds"] = round(time.monotonic() - start, 3)
        try:
            after = cache_stats(env)
            metrics.update(cache_summary(before, after))
            metrics["cache_stats_before"] = before
            metrics["cache_stats_after"] = after
        except (subprocess.CalledProcessError, ValueError) as error:
            metrics["cache_stats_error"] = str(error)
        (build / "host-metrics.json").write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
        lines = ["### Host regression performance", "", f"- Status: **{metrics['status']}**",
                 f"- Compiler jobs: {jobs}; test jobs: 1 (serial)",
                 f"- Registered / executed tests: {metrics.get('registered_tests', 0)} / {metrics.get('executed_tests', 0)}"]
        for name, seconds in metrics["phases_seconds"].items():
            lines.append(f"- {name}: {seconds:.3f} s")
        lines.extend([f"- Total: {metrics['total_seconds']:.3f} s",
                      f"- Host cache hit: {metrics.get('cache_hit_percent')}%",
                      f"- Host cache counters (this invocation): `{metrics.get('cache_delta', {})}`", ""])
        report = "\n".join(lines)
        (build / "host-test-report.md").write_text(report, encoding="utf-8")
        print(report, flush=True)
        if summary := env.get("GITHUB_STEP_SUMMARY"):
            with Path(summary).open("a", encoding="utf-8") as out:
                out.write(report + "\n")
    return code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--check-removed-transport", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        gib = 1024**3
        for inputs, expected in [((16, 16*gib, 4), 4), ((2, 16*gib, 4), 2),
                                 ((16, 2*gib, 4), 1), ((16, 0, 4), 1), ((16, 16*gib, 2), 2)]:
            if parallel_budget(*inputs) != expected:
                raise AssertionError((inputs, expected))
        try:
            parallel_budget(4, 8*gib, 0)
        except ValueError:
            pass
        else:
            raise AssertionError("Zero compiler jobs must be rejected")
        if len(REQUIRED_TESTS) != 34:
            raise AssertionError("Unexpected migration inventory")
        before = {"direct_cache_hit": 100, "preprocessed_cache_hit": 50, "cache_miss": 30}
        after = {"direct_cache_hit": 178, "preprocessed_cache_hit": 58, "cache_miss": 44}
        measured = cache_summary(before, after)
        if measured["cache_hit_percent"] != 86.0 or measured["cacheable_calls"] != 100:
            raise AssertionError("ccache counter names or delta calculation regressed")
        if cache_summary(before, before)["cache_hit_percent"] is not None:
            raise AssertionError("A no-op build must not report a fabricated hit rate")
        try:
            cache_summary({}, after)
        except ValueError:
            pass
        else:
            raise AssertionError("Unknown ccache counters must not silently become zero")
        print("Host CI resource budget, cache counters and migration inventory: PASS")
        return 0
    if args.check_removed_transport:
        check_removed_transport()
        return 0
    return run()


if __name__ == "__main__":
    sys.exit(main())


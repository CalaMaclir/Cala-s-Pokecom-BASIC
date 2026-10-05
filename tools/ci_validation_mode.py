#!/usr/bin/env python3
"""Select FAST/FULL without confusing PR merge refs with branch names."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import tempfile
from collections.abc import Mapping

# A version-only branch is a release branch; v0.91-stage* remains FAST.
RELEASE_BRANCH = re.compile(r"v\d+(?:\.\d+){1,2}(?:-release(?:-[A-Za-z0-9._-]+)?)?")


def is_release_branch(name: str) -> bool:
    return bool(RELEASE_BRANCH.fullmatch(name)) or (
        name.startswith("release/") and len(name) > len("release/")
    )


def pr_has_label(env: Mapping[str, str], name: str) -> bool:
    path = env.get("GITHUB_EVENT_PATH")
    if not path:
        return False
    try:
        event = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError):
        return False
    labels = event.get("pull_request", {}).get("labels", [])
    return any(label.get("name") == name for label in labels)


def select_mode(env: Mapping[str, str]) -> tuple[bool, str]:
    event = env.get("GITHUB_EVENT_NAME", "")
    if event == "workflow_dispatch" and env.get("CI_REQUESTED_FULL", "false").lower() == "true":
        return True, "explicit manual FULL validation"
    if event in {"pull_request", "pull_request_target"}:
        head = env.get("GITHUB_HEAD_REF", "")
        base = env.get("GITHUB_BASE_REF", "")
        if pr_has_label(env, "full-validation"):
            return True, "explicit PR full-validation label"
        if head == "v0.94-stage10-11-final":
            return True, "v0.94 Stage 10-11 release candidate validation"
        if head == "v0.93-stage6-release":
            return True, "v0.93 Stage 6 release candidate validation"
        if head == "v0.93-integration":
            return True, "v0.93 accepted stack integration validation"
        if head == "v0.93-stage5ab-performance-integer-research":
            return True, "v0.93 Stage 5 normal and experimental integration validation"
        if head == "v0.93-stage4-hardware-stability":
            return True, "v0.93 Stage 4 hardware stability validation"
        if head=="v0.93-stage3c3d3e-control-data-select":
            return True,"v0.93 Stage 3C/3D/3E integration validation"
        if not head:
            return True, "missing PR head ref: fail safe to FULL"
        if is_release_branch(head) or is_release_branch(base):
            return True, "release source or target branch"
        return False, "ordinary development PR"
    ref = env.get("GITHUB_REF_NAME", "")
    if ref == "v0.93-stage6-release":
        return True, "v0.93 Stage 6 release candidate validation"
    if env.get("GITHUB_REF_TYPE") == "tag":
        return True, "tag validation"
    if ref == "main" or is_release_branch(ref):
        return True, "main or release branch"
    if not ref:
        return True, "missing ref: fail safe to FULL"
    return False, "ordinary development branch"


def performance_mode(env: Mapping[str, str]) -> str:
    # One resolved mode, independent of FAST/FULL. PR label is an explicit
    # connector-accessible request; pushes never inherit it.
    if env.get("GITHUB_EVENT_NAME") == "workflow_dispatch":
        mode=env.get("CI_PERFORMANCE_MODE", "off")
        if mode not in ("off", "report"):raise ValueError("Unknown performance mode")
        return mode
    if env.get("GITHUB_EVENT_NAME") == "pull_request":
        if pr_has_label(env, "performance-report"):
            return "report"
    return "off"


def self_test() -> None:
    cases: list[tuple[dict[str, str], bool]] = []
    for branch, expected in [
        ("main", True), ("v0.8", True), ("v0.90", True),
        ("v0.91", True), ("v1.2.3", True),
        ("v0.91-release-prep", True), ("v0.91-release-final", True),
        ("release/v0.92", True), ("v0.91-stage1-serial-performance", False),
        ("v0.91-keyboard-i2c-recovery", False), ("v0.91-ci-host-build", False),
        ("feature/release-notes", False), ("v0.91x", False),
    ]:
        cases.append(({"GITHUB_EVENT_NAME": "push", "GITHUB_REF_NAME": branch}, expected))
        cases.append(({
            "GITHUB_EVENT_NAME": "pull_request", "GITHUB_REF_NAME": "53/merge",
            "GITHUB_HEAD_REF": branch, "GITHUB_BASE_REF": "main",
        }, expected and branch != "main"))
    cases.extend([
        ({"GITHUB_EVENT_NAME": "pull_request", "GITHUB_HEAD_REF": "v0.94-stage10-11-final", "GITHUB_BASE_REF": "main"}, True),
        ({"GITHUB_EVENT_NAME":"pull_request", "GITHUB_HEAD_REF":"v0.93-additional-a1-wifi-retry-settle",
          "GITHUB_BASE_REF":"main"}, False),
        ({"GITHUB_EVENT_NAME":"pull_request", "GITHUB_HEAD_REF":"v0.93-stage6-release",
          "GITHUB_BASE_REF":"main"}, True),
        ({"GITHUB_EVENT_NAME":"push", "GITHUB_REF_NAME":"v0.93-stage6-release"}, True),
        ({"GITHUB_EVENT_NAME":"pull_request", "GITHUB_HEAD_REF":"v0.93-integration",
          "GITHUB_BASE_REF":"main"}, True),
        ({"GITHUB_EVENT_NAME":"pull_request", "GITHUB_HEAD_REF":"v0.93-stage5ab-performance-integer-research",
          "GITHUB_BASE_REF":"v0.93-stage4-hardware-stability"}, True),
        ({"GITHUB_EVENT_NAME":"pull_request", "GITHUB_HEAD_REF":"v0.93-stage4-hardware-stability",
          "GITHUB_BASE_REF":"main"}, True),
        ({"GITHUB_EVENT_NAME":"pull_request","GITHUB_HEAD_REF":"v0.93-stage3c3d3e-control-data-select",
          "GITHUB_BASE_REF":"v0.93-stage3a3b-math-intops"}, True),
        ({"GITHUB_EVENT_NAME": "pull_request", "GITHUB_HEAD_REF": "feature/test",
          "GITHUB_BASE_REF": "v0.91", "GITHUB_REF_NAME": "53/merge"}, True),
        ({"GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF_NAME": "feature/test",
          "CI_REQUESTED_FULL": "true"}, True),
        ({"GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF_NAME": "feature/test",
          "CI_REQUESTED_FULL": "false"}, False),
        ({"GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF_NAME": "main",
          "CI_REQUESTED_FULL": "false"}, True),
        ({"GITHUB_EVENT_NAME": "push", "GITHUB_REF_NAME": "v0.91",
          "GITHUB_REF_TYPE": "tag"}, True),
        ({"GITHUB_EVENT_NAME": "push", "GITHUB_REF_NAME": "feature/test",
          "CI_REQUESTED_FULL": "true"}, False),
        ({"GITHUB_EVENT_NAME": "pull_request", "GITHUB_REF_NAME": "53/merge"}, True),
        ({}, True),
    ])
    for env, expected in cases:
        actual, reason = select_mode(env)
        if actual != expected:
            raise AssertionError(f"{env}: expected {expected}, got {actual}: {reason}")
    with tempfile.TemporaryDirectory(prefix="cpb-ci-policy-") as folder:
        event_path = Path(folder) / "event.json"
        event_path.write_text(json.dumps({
            "pull_request": {"labels": [{"name": "full-validation"}]}
        }))
        actual, reason = select_mode({
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_HEAD_REF": "feature/additional-stage",
            "GITHUB_BASE_REF": "main",
            "GITHUB_EVENT_PATH": str(event_path),
        })
        if not actual:
            raise AssertionError(f"full-validation label did not request FULL: {reason}")
        if performance_mode({
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_EVENT_PATH": str(event_path),
        }) != "off":
            raise AssertionError("full-validation label must not enable timing reports")
        event_path.write_text(json.dumps({
            "pull_request": {"labels": [{"name": "performance-report"}]}
        }))
        if performance_mode({
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_EVENT_PATH": str(event_path),
        }) != "report":
            raise AssertionError("performance-report label did not enable timing")
    print(f"CI validation policy: {len(cases) + 3} cases PASS")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    full, reason = select_mode(os.environ)
    performance=performance_mode(os.environ)
    print(f"PERFORMANCE_MODE={performance}")
    value = str(full).lower()
    print(f"FULL_VALIDATION={value} ({reason})")
    if path := os.environ.get("GITHUB_ENV"):
        with Path(path).open("a", encoding="utf-8") as out:
            out.write(f"FULL_VALIDATION={value}\nPERFORMANCE_MODE={performance}\n")
    if path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(path).open("a", encoding="utf-8") as out:
            out.write(f"### Validation mode\n\n- Mode: **{'FULL' if full else 'FAST'}**\n- Reason: {reason}\n- Host timing mode: **{performance}** (independent of FULL)\n")


if __name__ == "__main__":
    main()


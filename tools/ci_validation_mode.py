#!/usr/bin/env python3
"""Select FAST/FULL without confusing PR merge refs with branch names."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
from collections.abc import Mapping

# A version-only branch is a release branch; v0.91-stage* remains FAST.
RELEASE_BRANCH = re.compile(r"v\d+(?:\.\d+){1,2}(?:-release(?:-[A-Za-z0-9._-]+)?)?")


def is_release_branch(name: str) -> bool:
    return bool(RELEASE_BRANCH.fullmatch(name)) or (
        name.startswith("release/") and len(name) > len("release/")
    )


def select_mode(env: Mapping[str, str]) -> tuple[bool, str]:
    event = env.get("GITHUB_EVENT_NAME", "")
    if event == "workflow_dispatch" and env.get("CI_REQUESTED_FULL", "false").lower() == "true":
        return True, "explicit manual FULL validation"
    if event in {"pull_request", "pull_request_target"}:
        head = env.get("GITHUB_HEAD_REF", "")
        base = env.get("GITHUB_BASE_REF", "")
        if not head:
            return True, "missing PR head ref: fail safe to FULL"
        if is_release_branch(head) or is_release_branch(base):
            return True, "release source or target branch"
        return False, "ordinary development PR"
    ref = env.get("GITHUB_REF_NAME", "")
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
        path=env.get("GITHUB_EVENT_PATH")
        if path:
            event=json.loads(Path(path).read_text())
            labels=event["pull_request"].get("labels",[])
            if any(label["name"]=="performance-report" for label in labels):return "report"
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
    print(f"CI validation policy: {len(cases)} cases PASS")


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


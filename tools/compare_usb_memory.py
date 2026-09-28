"""Compare two reports built with the same SDK and ARM toolchain."""

import re
import sys
from pathlib import Path


def section(report, name):
    match = re.search(
        rf"^{re.escape(name)}\s+(\d+)\s",
        report,
        re.MULTILINE,
    )
    if not match:
        raise SystemExit(f"missing section {name}")
    return int(match.group(1))


def object_size(report, symbol):
    match = re.search(
        rf"^\d+\s+(\d+)\s+\w\s+{re.escape(symbol)}$",
        report,
        re.MULTILINE,
    )
    return int(match.group(1)) if match else None


def sizes(path):
    report = Path(path).read_text()
    text = section(report, ".text")
    rodata = section(report, ".rodata")
    arm_exidx = section(report, ".ARM.exidx")
    binary_info = section(report, ".binary_info")
    data = section(report, ".data")
    bss = section(report, ".bss")
    vector_table = section(report, ".ram_vector_table")
    uninitialized = section(report, ".uninitialized_data")
    flash_end = section(report, ".flash_end")
    repl = int(
        re.search(
            r"^\d+\s+(\d+)\s+\w\s+memory_size_repl$",
            report,
            re.MULTILINE,
        )[1]
    )
    stack_frames = [
        int(match.group(1))
        for match in re.finditer(
            r"\t(\d+)\t(?:static|dynamic)\s*$",
            report,
            re.MULTILINE,
        )
    ]
    result = {
        "Flash load image": (
            text + rodata + arm_exidx + binary_info + data + flash_end
        ),
        ".text": text,
        ".rodata": rodata,
        ".data": data,
        ".bss": bss,
        ".data + .bss": data + bss,
        "Static SRAM sections": (
            vector_table + uninitialized + data + bss
        ),
        "Max compiler stack frame": max(stack_frames, default=0),
        "sizeof(Repl)": repl,
    }
    for label, symbol in (
        ("sizeof(EditorModel)", "memory_size_editor_model"),
        ("sizeof(FullScreenEditor)", "memory_size_editor_heap"),
        ("sizeof(ProgramStore)", "memory_size_program"),
    ):
        value = object_size(report, symbol)
        if value is not None:
            result[label] = value
    return result


before, after = (sizes(path) for path in sys.argv[1:])
print("Version 0.89 CPU/LED: ARM memory comparison (bytes)")
print(
    "\nBaseline: `v0.88` at "
    "`bfafc35b1b6a60058c9f582732a8c7b74bb2e669`; "
    "same CI toolchain / Pico SDK 2.3.1."
)
print("\n| Item | v0.88 | v0.89 | Delta |")
print("| --- | ---: | ---: | ---: |")
for name in before:
    print(
        f"| {name} | {before[name]} | {after[name]} | "
        f"{after[name] - before[name]:+d} |"
    )
print(
    "\nStatic allocations and compiler-estimated stack frames only; "
    "runtime heap/stack high-water marks require hardware measurement."
)
print(f"\nRepl object delta: {after['sizeof(Repl)'] - before['sizeof(Repl)']:+d} bytes")

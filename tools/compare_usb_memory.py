"""Compare reports built with the same SDK, ARM toolchain and build number."""
import re
import sys
from pathlib import Path


def sizes(path):
    report = Path(path).read_text()
    data = int(re.search(r"^\.data\s+(\d+)\s", report, re.M)[1])
    bss = int(re.search(r"^\.bss\s+(\d+)\s", report, re.M)[1])
    repl = int(re.search(r"^\d+\s+(\d+)\s+\w\s+memory_size_repl$", report, re.M)[1])
    return {".data": data, ".bss": bss, ".data + .bss": data + bss, "sizeof(Repl)": repl}


before, after = (sizes(path) for path in sys.argv[1:])
print("USB MSC Stage 2: ARM SRAM comparison (bytes)")
print("\nStage 1 baseline: `cc62e7db930efe370beb2f5ca10be21d8da3bb73`; same CI toolchain / SDK 2.3.1.")
print("\n| Item | Before | After | Delta |")
print("| --- | ---: | ---: | ---: |")
for name in before:
    print(f"| {name} | {before[name]} | {after[name]} | {after[name] - before[name]:+d} |")
print("\nStatic allocations only; heap/stack runtime high-water marks require hardware measurement.")
if before["sizeof(Repl)"] != after["sizeof(Repl)"]:
    raise SystemExit("USB ownership work must not change Repl size")

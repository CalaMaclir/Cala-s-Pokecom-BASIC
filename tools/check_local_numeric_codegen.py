"""Verify new local fusion keeps separate ARM float rounding in the real ELF."""
import argparse, json, re, subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser()
    p.add_argument("elf")
    p.add_argument("--output",default="build/local-numeric-codegen")
    a=p.parse_args()
    assembly=subprocess.check_output(["arm-none-eabi-objdump","-d","-C",a.elf],text=True)
    blocks=[];active=None
    for line in assembly.splitlines():
        label=re.match(r"^[0-9a-fA-F]+ <(.+)>:$",line)
        if label:
            active=None
            if any(name in label.group(1) for name in ("evaluate_local_numeric(","evaluate_local_gray(","VM::run_impl<true>(")):
                active={"symbol":label.group(1),"lines":[line]}
                blocks.append(active)
        elif active is not None:
            active["lines"].append(line)
    assert any("VM::run_impl<true>(" in b["symbol"] for b in blocks),"inlined numeric-local VM body is missing from ARM codegen evidence"
    assert any("evaluate_local_gray(" in b["symbol"] for b in blocks),"local-gray helper is missing from ARM codegen evidence"
    fused=re.compile(r"\bv(?:fma|fms|fnma|fnms)(?:\.[a-zA-Z0-9]+)?\b")
    failures=[line for block in blocks for line in block["lines"] if fused.search(line)]
    result={"elf":a.elf,"symbols":[b["symbol"] for b in blocks],
        "policy":"new local fusion must not contract separate float operations into FMA",
        "fma_instructions":failures,"pass":not failures}
    out=Path(a.output)
    out.with_suffix(".json").write_text(json.dumps(result,indent=2)+"\n")
    out.with_suffix(".asm").write_text("\n".join(line for block in blocks for line in block["lines"])+"\n")
    print(json.dumps(result,indent=2))
    if failures:
        raise SystemExit("ARM numeric-local fusion changed rounding through FMA")
if __name__=="__main__":main()


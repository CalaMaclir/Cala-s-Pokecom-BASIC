"""Compare Stage 1 and current Classic cases with paired raw evidence."""
import argparse
import json
import re
import statistics
from performance_policy import compare_samples, outcome, same, verify_source, STAGE1_SHA, error_record
import subprocess
from pathlib import Path

def memory(path):
    text=Path(path).read_text()
    values={name:int(size) for name,size in re.findall(r"^\s*(\.data|\.bss)\s+(\d+)\s",text,re.M)}
    values.update({name:int(size) for size,name in re.findall(r"\b(\d+)\s+[A-Za-z]\s+memory_size_(\w+)\s*$",text,re.M)})
    for label,pattern in (("vm_native_stack","VM::run_impl"),("compiler_native_stack","BasicCompiler::compile_source")):
        rows=[line for line in text.splitlines() if pattern in line and "\t" in line]
        sizes=[int(line.split("\t")[-2]) for line in rows]
        if sizes:values[label]=max(sizes)
    return values

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--baseline",required=True)
    parser.add_argument("--current",required=True)
    parser.add_argument("--baseline-memory",required=True)
    parser.add_argument("--current-memory",required=True)
    parser.add_argument("--output",required=True)
    args=parser.parse_args()
    from paired_benchmark import environment, measure
    verify_source(args.baseline, STAGE1_SHA)
    runner=environment()
    sources={"examples/stage3/arithmetic-classic.bas":"arithmetic",
        "examples/stage3/numeric-classic.bas":"numeric-functions",
        "examples/mandel_text.bas":"mandel_text"}
    runs,schedule=measure({"Stage 1":args.baseline,"Current":args.current},
        {case:("Stage 1","Current") for case in sources})
    for group in runs.values():
        for run in group:
            for case in run["cases"]:
                case["source_path"]=case["name"]
                case["name"]=sources[case["name"]]
    result={"baseline_sha":"35367d7b6156fce92fbebbee7407261041e7af28","runs":runs,"runner":runner,"schedule":schedule,
            "memory":{"Stage 1":memory(args.baseline_memory),"Current":memory(args.current_memory)}}
    report=["# v0.92 Stage 1 / current Classic evidence","",
            "Baseline: 35367d7b6156fce92fbebbee7407261041e7af28; same runner, SDK 2.3.1 and build flags.",
            "Host timing: -O3, steady_clock, nine case/metric-paired rounds on one fixed CPU; every raw sample retained.",
            "Timing is host evidence; hardware acceptance must repeat Classic benchmarks on PicoCalc.","",
            "| Case | Measure | Stage 1 (us) | Current (us) | Delta |",
            "|---|---|---:|---:|---:|"]
    failures=[];investigate=[];comparisons=[]
    for index,base_case in enumerate(runs["Stage 1"][0]["cases"]):
        for key in ("dispatches","logical_ops","ops","output_hash"):
            expected=base_case[key]
            same(all(run["cases"][index][key]==expected for group in runs.values() for run in group), True, (base_case["name"],key))
        for metric in ("compile_us","vm_us"):
            baseline_samples=[x for run in runs["Stage 1"] for x in run["cases"][index][metric.replace("_us","_raw_us")]]
            current_samples=[x for run in runs["Current"] for x in run["cases"][index][metric.replace("_us","_raw_us")]]
            baseline,current,delta=compare_samples(baseline_samples,current_samples)
            comparisons.append({"case":base_case["name"],"metric":metric,"baseline":baseline,"current":current,"delta_percent":delta,
                "baseline_mad":statistics.median(abs(x-baseline) for x in baseline_samples),
                "current_mad":statistics.median(abs(x-current) for x in current_samples),
                "baseline_min":min(baseline_samples),"baseline_max":max(baseline_samples),
                "current_min":min(current_samples),"current_max":max(current_samples)})
            report.append(f"| {base_case['name']} | {metric} | {baseline:.3f} | {current:.3f} | {delta:+.2f}% |")
            if delta>=5:investigate.append(f"{base_case['name']} {metric}: {delta:+.2f}%")
            if delta>=10:failures.append(f"{base_case['name']} {metric}: {delta:+.2f}%")
    result["comparisons"]=comparisons
    result.update(outcome(comparisons))
    result["failures"]=failures  # historical timing-alert field, advisory only
    report+=["","Output fingerprints, compiled op counts, dispatches and logical-op counts agree in every run.","",
             "| ARM memory / stack (bytes) | Stage 1 | Current | Delta |","|---|---:|---:|---:|"]
    baseline=result["memory"]["Stage 1"];current=result["memory"]["Current"]
    for name in sorted(set(baseline)|set(current)):
        left=baseline.get(name);right=current.get(name)
        report.append(f"| {name} | {left if left is not None else '—'} | {right if right is not None else '—'} | {right-left if left is not None and right is not None else '—'} |")
    report+=["","Native stack numbers are compiler .su bounds, not runtime high-water marks.",
             "Frame probes include header plus exact numeric/string locals. Depth-16 probes are summed heap allocations.",
             "String return buffers are lazy: at most 192 x 128 bytes plus 192 pointers during an execution.",
             "SRAM exhaustion produces OUT OF MEMORY and releases all active frames.","",
             ">=5% investigations: "+("; ".join(investigate) if investigate else "none"),
             ">=10% advisory timing alerts: "+("; ".join(failures) if failures else "none"),
             "Timing status: "+result["performance_status"]+"; correctness: PASS; timing never blocks artifacts."]
    output=Path(args.output);output.parent.mkdir(parents=True,exist_ok=True)
    output.with_suffix(".json").write_text(json.dumps(result,indent=2)+"\n")
    output.with_suffix(".md").write_text("\n".join(report)+"\n")
    print("\n".join(report))

if __name__=="__main__":
    try:main()
    except (AssertionError, ValueError, KeyError, TypeError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        import sys
        if "--output" in sys.argv:
            path=Path(sys.argv[sys.argv.index("--output")+1]).with_suffix(".json")
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(json.dumps(error_record(error),indent=2)+"\n")
        raise


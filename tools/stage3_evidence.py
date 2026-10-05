"""Shared short/full VM binary; alternating revisions; preserve every timing sample."""
import argparse, json, statistics, subprocess, platform, os, tempfile
from pathlib import Path
from stage2_evidence import memory as legacy_memory
from paired_benchmark import environment, measure
from performance_policy import compare_samples, outcome, same, verify_source, STAGE1_SHA, STAGE2_SHA, error_record
def memory(path):
    import re
    values=legacy_memory(path)
    text=Path(path).read_text()
    for key in ('DATA_PLUS_BSS_BYTES','FLASH_LOAD_BYTES'):
        match=re.search(r'^'+key+r' (\d+)',text,re.M)
        if match:values[key]=int(match[1])
    rows=[line for line in text.splitlines() if "evaluate_local_numeric" in line and "\t" in line]
    if rows:values["local_template_native_stack"]=max(int(line.split("\t")[-2]) for line in rows)
    for mode in ("false","true"):
        rows=[line for line in text.splitlines() if "VM::run_impl" in line and "UserFunctions = "+mode in line and "\t" in line]
        if rows:values["vm_native_stack_"+mode]=max(int(line.split("\t")[-2]) for line in rows)
    return values
def main():
    p=argparse.ArgumentParser()
    for n in ("stage1","stage2","current","stage1-memory","stage2-memory","current-memory","output"):
        p.add_argument("--"+n,required=True)
    p.add_argument("--baseline-only",action="store_true")
    p.add_argument("--accepted")
    p.add_argument("--accepted-memory")
    p.add_argument("--accepted-sha",default="2a7b1c568d7a6cbef8b9a34b438f05258f5ca84a")
    a=p.parse_args()
    names=("Stage 1","Stage 2","Current")
    binaries=dict(zip(names,(a.stage1,a.stage2,a.current)))
    baselines=("Stage 1","Stage 2")
    if a.accepted:
        if not a.accepted_memory:p.error("--accepted requires --accepted-memory")
        names=("Stage 1","Stage 2","Accepted 3AB","Current")
        binaries={"Stage 1":a.stage1,"Stage 2":a.stage2,"Accepted 3AB":a.accepted,"Current":a.current}
        baselines=("Stage 1","Stage 2","Accepted 3AB")
    verify_source(a.stage1, STAGE1_SHA)
    verify_source(a.stage2, STAGE2_SHA)
    if a.accepted:verify_source(a.accepted, a.accepted_sha)
    import hashlib
    binary_identity={}
    for n,binary in binaries.items():
        path=Path(binary)
        symbols=subprocess.check_output(["nm","-S","--size-sort","-C",str(path)],text=True)
        sections={}
        with tempfile.TemporaryDirectory() as temp:
            for section in (".text",".rodata",".data"):
                target=Path(temp)/section[1:]
                subprocess.run(["objcopy","--dump-section",section+"="+str(target),str(path)],check=True)
                sections[section]=hashlib.sha256(target.read_bytes()).hexdigest()
        binary_identity[n]={"sha256":hashlib.sha256(path.read_bytes()).hexdigest(),"sections":sections,
            "vm_symbols":[line for line in symbols.splitlines() if "VM::run_impl" in line]}
    identity_path=Path(a.output).with_name("stage3-binary-identity.json")
    identity_path.parent.mkdir(parents=True,exist_ok=True)
    identity_path.write_text(json.dumps(binary_identity,indent=2)+"\n")
    print("Benchmark binary/section identity:",json.dumps(binary_identity),flush=True)
    identical_sections = bool(a.accepted and binary_identity["Accepted 3AB"]["sections"]==binary_identity["Current"]["sections"])
    runner=environment()
    classic=["examples/mandel_text.bas","examples/picocalc_mand.bas"]
    classic += [f"examples/stage3/{group}-classic.bas" for group in ("arithmetic","numeric","fractal")]
    case_names={case:names for case in classic}
    for group in ("arithmetic","numeric","fractal"):
        for mode in ("colon","rows","function"):
            case_names[f"examples/stage3/{group}-{mode}.bas"]=tuple(n for n in names if n!="Stage 1")
    case_names["examples/stage3/tiny-function.bas"]=tuple(n for n in names if n!="Stage 1")
    runs,schedule=measure(binaries,case_names)
    def cases(n):return {c["name"]:c for c in runs[n][0]["cases"]}
    def samples(n,name,m):return [x for r in runs[n] for c in r["cases"] if c["name"]==name for x in c[m+"_raw_us"]]
    failures=[];investigate=[];comparisons=[]
    report=["# v0.92 Stage 3 measurements","",
        "Same runner/toolchain/flags; -O3, no sanitizer, no fast-math; canonical __FILE__ prefix. CPU affinity fixed; child ASLR disabled and accepted/current mapped image/heap/anonymous/stack addresses identical; nine alternating rounds per case/metric.",
        "One full batch warmup/metric/revision plus three calls/sample. Compile: 20000; VM: 1000 or 2 full 320x320 graphics; cache: 10000.",
        "compile = compile/reset/metadata; VM = existing IL plus VM initialization and captured output/graphics.",
        "cache miss = invalidation + compile + CRC/store; cache hit = restore/CRC/reset/metadata. Neither includes VM.",
        "Input construction/ProgramStore initialization excluded. PSRAM is a host 8 MiB memory model.",
        "Timing PROFILE OFF; op counts are a separate PROFILE COUNT run. ccache is only the C++ build cache.",
        "Host evidence only; native .su is static compiler evidence, never device high-water.","",
        "| Baseline | Case | Measure | Before us | Current us | Delta |","|---|---|---|---:|---:|---:|"]
    current=cases("Current")
    for n in names:
        for case in cases(n):
            for r in runs[n]:
                c=next(x for x in r["cases"] if x["name"]==case)
                for key in ("output_hash","graphics_hash","pixels","ops","dispatches","logical_ops","op_counts"):
                    same(c[key],cases(n)[case][key],(n,case,key,"unstable"))
    for baseline in baselines:
        for name,left in cases(baseline).items():
            right=current[name]
            for key in ("output_hash","graphics_hash","pixels"):
                same(left[key],right[key],(baseline,name,key))
            for m in ("compile","vm","cache_miss","cache_hit"):
                b=samples(baseline,name,m);c=samples("Current",name,m)
                bm,cm,delta=compare_samples(b,c)
                row=dict(baseline=baseline,case=name,metric=m,before_us=bm,current_us=cm,delta_percent=delta,
                         before_min=min(b),before_max=max(b),current_min=min(c),current_max=max(c),
                         before_mad=statistics.median(abs(x-bm) for x in b),
                         current_mad=statistics.median(abs(x-cm) for x in c))
                comparisons.append(row)
                report.append(f"| {baseline} | {Path(name).name} | {m} | {bm:.3f} | {cm:.3f} | {delta:+.2f}% |")
                if delta>=5:investigate.append(row)
                if delta>=10:failures.append(row)
    for n in tuple(n for n in names if n!="Stage 1"):
        for group in ("arithmetic","numeric","fractal"):
            variants=[cases(n)[f"examples/stage3/{group}-{mode}.bas"] for mode in ("classic","colon","rows","function")]
            for key in ("output_hash","graphics_hash","pixels"):
                same(len({v[key] for v in variants}),1,(n,group,key))
    import re
    enum=Path("include/il.hpp").read_text().split("enum class OpCode")[1].split("};")[0].split("{",1)[1]
    opcode_names=[x.strip() for x in re.sub(r"//[^\n]*","",enum).split(",") if x.strip()]
    def opcode_name(revision,index):
        if revision in ("Stage 1","Stage 2") and index==opcode_names.index("LOCAL_NUM_FUSED"):return "HALT"
        if revision=="Accepted 3AB" and "LOCAL_GRAY_PSET_MUL_INT" in opcode_names and index==opcode_names.index("LOCAL_GRAY_PSET_MUL_INT"):return "HALT"
        return opcode_names[index] if index<len(opcode_names) else str(index)
    fusions={}
    for n in names:
        fusions[n]={}
        for case in cases(n).values():
            fusions[n][case["name"]]={opcode_name(n,i):count for i,count in enumerate(case["op_counts"])
                if (count and 81<=i<=99) or (count and n in ("Current","Accepted 3AB") and opcode_name(n,i)=="LOCAL_NUM_FUSED")}
    report+=["","| Revision | Case | IL slots | Dispatch | Logical ops | User calls |","|---|---|---:|---:|---:|---:|"]
    for n in names:
        for c in cases(n).values():
            calls=c["op_counts"][int(os.environ["CPB_CALL_USER_OPCODE"])] if n!="Stage 1" else 0
            report.append(f"| {n} | {Path(c['name']).name} | {c['ops']} | {c['dispatches']} | {c['logical_ops']} | {calls} |")
    report+=["","## Executed fusion opcodes",""]
    for n in names:
        for case,counts in fusions[n].items():
            report.append(n+" "+Path(case).name+": "+", ".join(f"{k}={v}" for k,v in counts.items()))
    memory_paths={"Stage 1":a.stage1_memory,"Stage 2":a.stage2_memory,"Current":a.current_memory}
    if a.accepted:memory_paths["Accepted 3AB"]=a.accepted_memory
    memories={n:memory(memory_paths[n]) for n in names}
    report+=["","| Memory/stack bytes | "+" | ".join(names)+" | Stage 2 delta |",
              "|---|"+"---:|"*(len(names)+1)]
    for key in sorted(set().union(*(m.keys() for m in memories.values()))):
        vals=[memories[n].get(key) for n in names]
        left=memories["Stage 2"].get(key);right=memories["Current"].get(key)
        delta=right-left if right is not None and left is not None else None
        report.append("| "+key+" | "+" | ".join(str(x) if x is not None else "-" for x in vals+[delta])+" |")
    result=dict(stage1_sha="35367d7b6156fce92fbebbee7407261041e7af28",
        stage2_sha="a4b54b596a674dfa231c45743ff977376d8b62d4",
        accepted_sha=a.accepted_sha if a.accepted else None,
        checkout_sha=subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip(),
        toolchain=subprocess.check_output(["g++","--version"],text=True).splitlines()[0],machine=platform.platform(),
        flags="-O3 -ffunction-sections -fdata-sections; -UNDEBUG; no fast-math; PROFILE OFF",
        binary_identity=binary_identity,runner=runner,schedule=schedule,runs=runs,comparisons=comparisons,memory=memories,fusion_counts=fusions,investigate=investigate,failures=failures,
        identical_accepted_current_sections=identical_sections)
    result.update(outcome(comparisons))
    result["baseline_only"]=a.baseline_only
    report+=["",">=5%: "+("; ".join(f"{r['baseline']} {Path(r['case']).name}/{r['metric']} {r['delta_percent']:+.2f}%" for r in investigate) or "none"),
             ">=10% advisory timing alerts: "+str(len(failures)),
             "Timing status: "+result["performance_status"]+"; correctness: PASS. No time threshold blocks artifacts.",
             "Accepted/current load sections identical: "+str(identical_sections)+". Time differences alone do not prove a code regression."]
    out=Path(a.output);out.parent.mkdir(parents=True,exist_ok=True)
    out.with_suffix(".json").write_text(json.dumps(result,indent=2)+"\n")
    out.with_suffix(".md").write_text("\n".join(report)+"\n")
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




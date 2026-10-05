"""v0.93 Stage 1: frozen main versus current using the existing paired protocol."""
import argparse, hashlib, json, re, statistics, subprocess
from pathlib import Path
from paired_benchmark import environment, measure, METRICS
from performance_policy import compare_samples, outcome, same, verify_source, error_record
from stage3_evidence import memory

BASELINE = '1ee0477ca8b157e8e953cb4fecf48291705f3731'

def host_codegen(binary):
    """Inspect the actual benchmark executable; no instrumentation in VM timing."""
    assembly = subprocess.check_output(['objdump','-d','--no-show-raw-insn','-C',binary],text=True)
    selected = {}; name = None
    for line in assembly.splitlines():
        header = re.match(r'^[0-9a-f]+ <(.+)>:$',line)
        if header:
            symbol = header.group(1)
            name = symbol if ('rmb::VM::run_impl<false>' in symbol or
                             'rmb::BasicCompiler::compile' in symbol or
                             'optimize_program(' in symbol) else None
            if name: selected[name] = []
        elif name: selected[name].append(line)
    return selected

def main():
    p = argparse.ArgumentParser()
    for name in ('baseline', 'current', 'baseline-memory', 'current-memory', 'output'):
        p.add_argument('--'+name, required=True)
    a = p.parse_args()
    verify_source(a.baseline, BASELINE)
    cases = ['examples/mandel_text.bas', 'examples/picocalc_mand.bas']
    cases += [f'examples/stage3/{group}-{mode}.bas' for group in ('arithmetic','numeric','fractal')
              for mode in ('classic','colon','rows','function')]
    cases += ['examples/stage3/tiny-function.bas']
    binaries = {'v0.93 baseline': a.baseline, 'Current': a.current}
    codegen = {n:host_codegen(b) for n,b in binaries.items()}
    Path(a.output).with_name('v093-host-codegen-evidence.json').write_text(json.dumps(codegen,indent=2)+'\n')
    runner = environment()
    runs, schedule = measure(binaries, {c:tuple(binaries) for c in cases})
    indexed = {n:{c['name']:c for c in runs[n][0]['cases']} for n in binaries}
    comparisons = []
    report = ['# v0.93 Stage 1 paired measurements', '', 'Baseline: `'+BASELINE+'`.',
              'Host only; nine alternating rounds, existing warmups/repetitions/affinity/ASLR protocol. PROFILE OFF timing; separate PROFILE COUNT.', '',
              '| Case | Metric | Baseline us | Current us | Delta |', '|---|---|---:|---:|---:|']
    for case in cases:
        before, current = (indexed[n][case] for n in binaries)
        for key in ('output_hash','graphics_hash','pixels','logical_ops'):
            same(before[key], current[key], (case,key))
        for metric in METRICS:
            left, right = before[metric+'_raw_us'], current[metric+'_raw_us']
            bm, cm, delta = compare_samples(left,right)
            row = dict(baseline='v0.93 baseline',case=case,metric=metric,before_us=bm,current_us=cm,
                       delta_percent=delta,before_min=min(left),before_max=max(left),current_min=min(right),current_max=max(right),
                       before_mad=statistics.median(abs(v-bm) for v in left),current_mad=statistics.median(abs(v-cm) for v in right))
            comparisons.append(row)
            report.append(f'| {Path(case).name} | {metric} | {bm:.3f} | {cm:.3f} | {delta:+.2f}% |')
    ratios = {}
    for revision in binaries:
        values = indexed[revision]
        for group in ('arithmetic','numeric','fractal'):
            variants=[values[f'examples/stage3/{group}-{m}.bas'] for m in ('classic','colon','rows','function')]
            for key in ('output_hash','graphics_hash','pixels'):
                same(len({v[key] for v in variants}),1,(revision,group,key))
        ratios[revision] = statistics.median(values['examples/stage3/fractal-function.bas']['vm_raw_us']) / statistics.median(values['examples/stage3/fractal-rows.bas']['vm_raw_us'])
    counts={}
    report += ['', '| Revision | Case | IL slots | Dispatch | Logical ops |', '|---|---|---:|---:|---:|']
    for revision,binary in binaries.items():
        root=Path(binary).resolve().parents[1]
        enum=(root/'include/il.hpp').read_text().split('enum class OpCode')[1].split('};')[0].split('{',1)[1]
        names=[v.strip() for v in re.sub(r'//[^\n]*','',enum).split(',') if v.strip()]
        counts[revision]={}
        for case,c in indexed[revision].items():
            counts[revision][case]={names[i]:n for i,n in enumerate(c['op_counts']) if n and i<len(names)}
            report.append(f"| {revision} | {Path(case).name} | {c['ops']} | {c['dispatches']} | {c['logical_ops']} |")
    memories={n:memory(path) for n,path in zip(binaries,(a.baseline_memory,a.current_memory))}
    report += ['', 'FUNCTION / rows VM ratios: '+json.dumps(ratios), '', '| Memory / static stack bytes | Baseline | Current | Delta |', '|---|---:|---:|---:|']
    for key in sorted(set().union(*(v.keys() for v in memories.values()))):
        left,right=(memories[n].get(key) for n in binaries)
        report.append(f'| {key} | {left} | {right} | {right-left if left is not None and right is not None else None} |')
    for n,values in counts.items():
        report += ['', n+' fractal-function opcode counts: '+json.dumps(values['examples/stage3/fractal-function.bas'])]
    result=dict(comparison_kind="v093-stage1",baseline_sha=BASELINE,checkout_sha=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
                runs=runs,schedule=schedule,runner=runner,comparisons=comparisons,memory=memories,opcode_counts=counts,function_rows_vm_ratio=ratios,
                binaries={n:hashlib.sha256(Path(b).read_bytes()).hexdigest() for n,b in binaries.items()})
    result.update(outcome(comparisons))
    report += ['', 'Timing is advisory: '+result['performance_status']+'. Correctness PASS. No timing threshold fails CI.']
    out=Path(a.output)
    out.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
    out.with_suffix('.md').write_text('\n'.join(report)+'\n')
    print('\n'.join(report))

if __name__ == '__main__':
    try: main()
    except (AssertionError,ValueError,KeyError,TypeError,OSError,RuntimeError,subprocess.SubprocessError) as error:
        import sys
        if '--output' in sys.argv:
            Path(sys.argv[sys.argv.index('--output')+1]).with_suffix('.json').write_text(json.dumps(error_record(error),indent=2)+'\n')
        raise

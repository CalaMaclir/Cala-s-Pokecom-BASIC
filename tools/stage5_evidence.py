"""Stage 5: immutable sources, A/B and B/C kept separate, existing paired protocol."""
import argparse, hashlib, json, os, re, statistics, subprocess
from pathlib import Path
from paired_benchmark import environment, measure, METRICS
from performance_policy import same, positive, compare_samples, outcome
from stage3_evidence import memory

BASELINE = 'd2449c38971d9b04c777252ffe29ecc60bb6cffa'
REVISIONS = ('Stage 4', 'Normal', 'IntFOR experimental')
INTEGER_CASES = ['examples/v093/performance/'+n+'.bas' for n in
                 ('for-empty','for-sum','for-nested','for-step2','for-negative','for-function')]
CASES = ['examples/mandel_text.bas','examples/picocalc_mand.bas'] + [
    f'examples/stage3/{k}-{m}.bas' for k in ('arithmetic','numeric','fractal')
    for m in ('classic','colon','rows','function')] + ['examples/stage3/tiny-function.bas'] + \
    INTEGER_CASES + ['examples/v093/performance/data-select-exit.bas']

def opcode_names():
    text=Path('include/il.hpp').read_text().split('enum class OpCode')[1].split('};')[0].split('{',1)[1]
    return [v.strip() for v in re.sub(r'//[^\n]*','',text).split(',') if v.strip()]

def arm_memory(path):
    values=memory(path);text=Path(path).read_text()
    sections={n:int(v) for n,v in re.findall(r'^\s*(\.[A-Za-z0-9_.]+)\s+(\d+)\s+\d+\s*$',text,re.M)}
    values['static_sram']=sum(sections.get(k,0) for k in
        ('.data','.bss','.ram_vector_table','.uninitialized_data','.tdata','.tbss','.scratch_x','.scratch_y'))
    values['flash_load_image']=sum(sections.get(k,0) for k in
        ('.text','.rodata','.data','.ARM.exidx','.binary_info','.boot2','.vectors','.flash_end'))
    for component in ('basic_compiler','vm'):
        frames=[int(line.split('\t')[-2]) for line in text.splitlines()
                if '/src/core/'+component+'.cpp:' in line and '\t' in line]
        if frames:values[component+'_max_native_frame']=max(frames)
    return values

def host_identity(binary):
    import tempfile
    binary=Path(binary).resolve();result={}
    with tempfile.TemporaryDirectory() as temp:
        for section in ('.text','.rodata','.data'):
            target=Path(temp)/section[1:]
            subprocess.run(['objcopy','--dump-section',section+'='+str(target),str(binary),str(Path(temp)/'copy')],check=True)
            result[section]=hashlib.sha256(target.read_bytes()).hexdigest()
    return {'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'sections':result}

def assert_equivalent(cases):
    for key in ('name','output_hash','graphics_hash','pixels','logical_ops'):
        same(len({c[key] for c in cases}),1,('Stage 5 oracle',cases[0]['name'],key))

def profile(binaries):
    names=opcode_names();runs={};rankings={}
    for revision,binary in binaries.items():
        values=[];total={}
        for case in CASES:
            p=subprocess.run([str(Path(binary).resolve()),'--paired-case',case],
                input='',text=True,capture_output=True,check=True)
            value=json.loads(p.stdout)
            value['opcode_counts']={names[i]:n for i,n in enumerate(value['op_counts']) if n}
            for k,n in value['opcode_counts'].items(): total[k]=total.get(k,0)+n
            values.append(value)
        runs[revision]=[{'cases':values}]
        rankings[revision]=sorted(total.items(),key=lambda x:-x[1])
    for cases in zip(*(runs[n][0]['cases'] for n in binaries)): assert_equivalent(cases)
    return runs,rankings

def validate(data):
    same(data['comparison_kind'],'v093-stage5ab','Stage 5 evidence identity')
    same(data['baseline_sha'],BASELINE,'frozen Stage 4')
    same(data['correctness_status'],'PASS','correctness')
    same(set(data['runs']),set(REVISIONS),'three revisions')
    same(set(data['source_sha256']),set(CASES),'source inventory')
    indexed={n:{c['name']:c for c in data['runs'][n][0]['cases']} for n in REVISIONS}
    comparisons=[]
    for case in CASES:
        assert_equivalent([indexed[n][case] for n in REVISIONS])
        for a,b in zip(REVISIONS,REVISIONS[1:]):
            for metric in METRICS:
                left,right=(indexed[n][case][metric+'_raw_us'] for n in (a,b))
                same(len(left),9,'all before samples');same(len(right),9,'all after samples')
                for v in left+right:positive(v)
                bm,cm,delta=compare_samples(left,right)
                comparisons.append((a,b,case,metric,bm,cm,delta))
    same(len(data['comparisons']),len(comparisons),'comparison inventory')
    for actual,(a,b,c,m,bm,cm,d) in zip(data['comparisons'],comparisons):
        for k,v in dict(before=a,after=b,case=c,metric=m,before_us=bm,current_us=cm,delta_percent=d).items():
            same(actual[k],v,('derived comparison',k))
    return data['performance_status']

def main():
    p=argparse.ArgumentParser()
    for n in ('baseline','normal','experimental','output'):p.add_argument('--'+n,required=True)
    for n in ('baseline-memory','normal-memory','experimental-memory'):p.add_argument('--'+n)
    p.add_argument('--profile-only',action='store_true')
    a=p.parse_args();binaries=dict(zip(REVISIONS,(a.baseline,a.normal,a.experimental)))
    runs,rankings=profile(binaries)
    result={'comparison_kind':'v093-stage5ab','baseline_sha':BASELINE,
        'checkout_sha':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        'source_sha256':{c:hashlib.sha256(Path(c).read_bytes()).hexdigest() for c in CASES},
        'binary_sha256':{n:hashlib.sha256(Path(b).read_bytes()).hexdigest() for n,b in binaries.items()},
        'profile_runs':runs,'opcode_rankings':rankings,'correctness_status':'PASS',
        'cache_format':6,'hardware_vm_timing':'NOT MEASURED',
        'hardware_compile_cache_timing':'NOT MEASURED'}
    memories=(a.baseline_memory,a.normal_memory,a.experimental_memory)
    if all(memories):result['memory']={n:arm_memory(path) for n,path in zip(REVISIONS,memories)}
    if a.profile_only:
        Path(a.output).with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
        print('Stage 5 identical-source profiles / output / graphics / logical oracle: PASS')
        return
    result['host_compiler']=subprocess.check_output(['g++','--version'],text=True).splitlines()[0]
    result['host_binary_identity']={n:host_identity(b) for n,b in binaries.items()}
    result['runner']=environment()
    result['runs'],result['schedule']=measure(binaries,{c:REVISIONS for c in CASES})
    indexed={n:{c['name']:c for c in result['runs'][n][0]['cases']} for n in REVISIONS}
    comparisons=[];report=['# v0.93 Stage 5A/5B host measurements','',
        'Stage 4: `'+BASELINE+'`. Existing CPU-pinned persistent-process paired protocol.',
        'Nine alternating rounds; PROFILE OFF timings, separate PROFILE COUNT. Host timing is advisory.',
        'A/B = general optimization; B/C = experimental integer FOR. RP2350: NOT MEASURED.','',
        '| Before → after | Case | Metric | Before µs | After µs | Delta |',
        '|---|---|---|---:|---:|---:|']
    for case in CASES:
        assert_equivalent([indexed[n][case] for n in REVISIONS])
        for before,after in zip(REVISIONS,REVISIONS[1:]):
            for metric in METRICS:
                left,right=(indexed[n][case][metric+'_raw_us'] for n in (before,after))
                bm,cm,d=compare_samples(left,right)
                row=dict(before=before,after=after,case=case,metric=metric,before_us=bm,
                         current_us=cm,delta_percent=d)
                for label,values,med in (('before',left,bm),('current',right,cm)):
                    row[label+'_min']=min(values);row[label+'_max']=max(values)
                    row[label+'_mad']=statistics.median(abs(x-med) for x in values)
                comparisons.append(row)
                report.append(f'| {before} → {after} | {Path(case).name} | {metric} | {bm:.3f} | {cm:.3f} | {d:+.2f}% |')
    result['comparisons']=comparisons
    result.update(outcome(comparisons))
    result['integer_type_recommendation']='DEFER'
    result['integer_type_reason']='Host-only FOR-control research; float body slots/conversions remain; RP2350 evidence required.'
    result['function_rows_vm_ratio']={n:statistics.median(indexed[n]['examples/stage3/fractal-function.bas']['vm_raw_us'])/
        statistics.median(indexed[n]['examples/stage3/fractal-rows.bas']['vm_raw_us']) for n in REVISIONS}
    validate(result)
    report+=['','FUNCTION / rows VM ratios: '+json.dumps(result['function_rows_vm_ratio']),
             '', 'Stage 5C: **DEFER**. Integer loop control is not a typed integer variable implementation.']
    for n in REVISIONS:report+=['',n+' top opcodes: '+json.dumps(rankings[n][:20])]
    out=Path(a.output);out.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
    out.with_suffix('.md').write_text('\n'.join(report)+'\n')
    print('\n'.join(report))

if __name__=='__main__':main()

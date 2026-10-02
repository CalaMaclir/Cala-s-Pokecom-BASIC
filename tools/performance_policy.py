"""Host timing is advisory; malformed evidence and semantic differences are fatal."""
import argparse
import json
import math
import os
import statistics
import subprocess
from pathlib import Path

ACCEPTED_SHA = '2a7b1c568d7a6cbef8b9a34b438f05258f5ca84a'
STAGE1_SHA = '35367d7b6156fce92fbebbee7407261041e7af28'
STAGE2_SHA = 'a4b54b596a674dfa231c45743ff977376d8b62d4'

class SemanticMismatch(AssertionError):
    pass

def same(left, right, context):
    if left != right:
        raise SemanticMismatch(f'{context}: {left!r} != {right!r}')

def positive(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError(f'Invalid positive timing: {value!r}')
    return value

def compare_samples(before, current, expected_count=9):
    if len(before) != expected_count or len(current) != expected_count:
        raise ValueError('Missing timing samples')
    for value in before + current:
        positive(value)
    bm, cm = statistics.median(before), statistics.median(current)
    delta = (cm / bm - 1) * 100
    if not math.isfinite(delta):
        raise ValueError('Nonfinite timing difference')
    return bm, cm, delta

def outcome(comparisons):
    if not comparisons:
        raise ValueError('Missing comparisons')
    for row in comparisons:
        value = row['delta_percent']
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError('Invalid difference')
    warnings = [r for r in comparisons if r['delta_percent'] >= 5]
    alerts = [r for r in comparisons if r['delta_percent'] >= 10]
    return {'schema_version': 2, 'performance_mode': 'report',
            'performance_status': 'WARN' if warnings else 'PASS',
            'correctness_status': 'PASS', 'measurement_status': 'COMPLETE',
            # Compatibility field is advisory and must never be used as a build gate.
            'acceptance_gate': 'WARN' if warnings else 'PASS',
            'warnings': warnings, 'timing_alerts': alerts, 'host_only': True}

def off():
    return {'schema_version': 2, 'performance_mode': 'off', 'performance_status': 'NOT_RUN',
            'correctness_status': 'PASS', 'measurement_status': 'NOT_RUN',
            'acceptance_gate': 'NOT_RUN', 'warnings': [], 'timing_alerts': [], 'host_only': True}

def error_record(error):
    return {'schema_version': 2, 'performance_mode': 'report', 'performance_status': 'ERROR',
            'correctness_status': 'FAIL' if isinstance(error, SemanticMismatch) else 'ERROR',
            'measurement_status': 'ERROR', 'acceptance_gate': 'ERROR',
            'error': f'{type(error).__name__}: {error}', 'host_only': True}

def verify_source(binary, expected):
    root = Path(binary).resolve().parents[1]
    actual = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    same(actual, expected, 'baseline source SHA')
    return actual

def validate_evidence(data):
    if data.get('schema_version') != 2 or data.get('performance_mode') != 'report':
        raise ValueError('Unsupported measured evidence schema')
    if data.get('measurement_status') != 'COMPLETE' or data.get('correctness_status') != 'PASS':
        raise ValueError('Measurement or correctness failed')
    calculated = outcome(data['comparisons'])
    for key in ('performance_status', 'acceptance_gate', 'warnings', 'timing_alerts'):
        same(data[key], calculated[key], key)
    if data.get('accepted_sha'):
        same(data['accepted_sha'], ACCEPTED_SHA, 'accepted SHA')
    if 'stage1_sha' in data:
        same(data['stage1_sha'], STAGE1_SHA, 'Stage 1 SHA')
        same(data['stage2_sha'], STAGE2_SHA, 'Stage 2 SHA')
        if not any(r['baseline'] == 'Accepted 3AB' for r in data['comparisons']):
            raise ValueError('Missing accepted baseline comparisons')
    else:
        same(data['baseline_sha'], STAGE1_SHA, 'Stage 1 SHA')
    # Recompute medians/deltas and required coverage, never trust status alone.
    runs = data['runs']
    baselines=('Stage 1','Stage 2','Accepted 3AB') if 'stage1_sha' in data else ('Stage 1',)
    same(set(runs),set(baselines)|{'Current'},'measured revisions')
    metrics=('compile','vm','cache_miss','cache_hit') if 'stage1_sha' in data else ('compile_us','vm_us')
    expected=set()
    for revision in baselines+('Current',):
        same(len(runs[revision]),1,'measured run count')
        cases=runs[revision][0]['cases']
        same(len({c['name'] for c in cases}),len(cases),'duplicate measured case')
        if not cases:raise ValueError('Missing measured cases')
        for case in cases:
            for key in ('output_hash','graphics_hash','pixels') if 'stage1_sha' in data else ('output_hash','dispatches','logical_ops','ops'):
                if revision != 'Current':
                    matches=[c for c in runs['Current'][0]['cases'] if c['name']==case['name']]
                    if len(matches)!=1:raise ValueError('Missing current measured case')
                    right=matches[0]
                    same(case[key],right[key],(revision,case['name'],key))
            if revision!='Current':
                expected.update((revision,case['name'],metric) for metric in metrics)
    actual={(r.get('baseline') if isinstance(r.get('baseline'),str) else 'Stage 1',r['case'],r['metric']) for r in data['comparisons']}
    same(actual,expected,'required comparison coverage')
    same(len(actual),len(data['comparisons']),'duplicate comparisons')
    for row in data['comparisons']:
        baseline = row.get('baseline') if isinstance(row.get('baseline'), str) else 'Stage 1'
        def samples(revision):
            cases = [c for r in runs[revision] for c in r['cases'] if c['name'] == row['case']]
            if len(cases) != 1:
                raise ValueError('Missing or duplicate measured case')
            return cases[0][row['metric'].removesuffix('_us') + '_raw_us']
        bm, cm, delta = compare_samples(samples(baseline), samples('Current'))
        same(bm, row.get('before_us', row.get('baseline')), 'baseline median')
        same(cm, row.get('current_us', row.get('current')), 'current median')
        same(delta, row['delta_percent'], 'difference')
    return data['performance_status']

def load_summary(directory, mode):
    if mode == 'off':
        return off()
    if mode != 'report':
        raise ValueError('Unknown performance mode')
    classic = json.loads((directory / 'stage2-evidence.json').read_text())
    full = json.loads((directory / 'stage3-evidence.json').read_text())
    statuses = {'classic': validate_evidence(classic), 'full_compile_vm_cache': validate_evidence(full)}
    return {'schema_version': 2, 'performance_mode': mode, 'performance_status':
            'WARN' if 'WARN' in statuses.values() else 'PASS', 'measurement_status': 'COMPLETE',
            'correctness_status': 'PASS', 'reports': statuses, 'host_only': True}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--mode', choices=('off', 'report'), required=True)
    p.add_argument('--directory', type=Path, default=Path('build'))
    a = p.parse_args()
    try:
        result = load_summary(a.directory, a.mode)
    except (AssertionError, ValueError, KeyError, TypeError, OSError) as error:
        (a.directory / 'performance-status.json').write_text(json.dumps(error_record(error), indent=2) + '\n')
        raise
    (a.directory / 'performance-status.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if path := os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(path, 'a') as f:
            f.write(f"\n### Host performance report\n\nMode: {a.mode}; timing: **{result['performance_status']}**; correctness: **PASS**. Host measurements do not establish PicoCalc device speed.\n")
    if result['performance_status'] == 'WARN':
        print('::warning::Host timing threshold exceeded; inspect retained raw data. Correctness passed.')

if __name__ == '__main__':
    main()

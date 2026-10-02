"""Current-only output/graphics oracle from frozen, accepted 3AB evidence. No timing."""
import json
import subprocess
import sys
from pathlib import Path
from performance_policy import same, ACCEPTED_SHA

def validate(data, fixture):
    same(fixture['accepted_sha'], ACCEPTED_SHA, 'oracle source')
    cases = {c['name']: c for c in data['cases']}
    same(len(cases), len(data['cases']), 'duplicate cases')
    same(set(cases), set(fixture['cases']), 'required cases')
    for name, expected in fixture['cases'].items():
        for key, value in expected.items():
            same(cases[name][key], value, (name, key))
    for group in ('arithmetic', 'numeric', 'fractal'):
        for key in ('output_hash', 'graphics_hash', 'pixels'):
            variants = [cases[f'examples/stage3/{group}-{mode}.bas'][key]
                        for mode in ('classic', 'colon', 'rows', 'function')]
            same(len(set(variants)), 1, (group, key, 'Classic/Structured semantics'))

def main():
    data = json.loads(subprocess.check_output([sys.argv[1], '--verify-only'], text=True))
    fixture = json.loads((Path(__file__).resolve().parents[1] / 'tests/benchmark_expected.json').read_text())
    validate(data, fixture)
    output = Path('build-host/benchmark-correctness.json')
    output.write_text(json.dumps({'correctness_status':'PASS','performance_status':'NOT_RUN',
                                'accepted_sha':ACCEPTED_SHA,'cases':data['cases']}, indent=2)+'\n')
    print('Accepted output/graphics fingerprints, variants and cache restoration: PASS; timing NOT_RUN')

if __name__ == '__main__':
    main()

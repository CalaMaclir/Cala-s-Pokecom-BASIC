"""Exercise package and ZIP integrity checks with corruption/inclusion failures."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_check', ROOT/'tools/validate_release_package.py')
module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
sha = lambda data: hashlib.sha256(data).hexdigest()
files = {'build/CPokecombasic.uf2': b'fixture firmware',
         'flash-cpb.cmd': b'set UF2=%~dp0build\\CPokecombasic.uf2',
         'README.md': b'0.94', 'README.en.md': b'0.94'}
manuals = []
for name in ('Install', 'System', 'Reference'):
    source, output = f'docs/{name}.md', f'docs/{name}.pdf'
    files[source], files[output] = b'0.94 manual', b'%PDF-fixture'
    manuals.append({'source': source, 'output': output,
                    'source_sha256': sha(files[source]), 'output_sha256': sha(files[output])})
manifest = {'version': '0.94', 'manuals': manuals}
files['docs/manuals-manifest.json'] = json.dumps(manifest).encode()
for name in ('release-notes', 'release-checklist', 'known-limitations'):
    files[f'docs/release/v0.94-{name}.md'] = b'0.94'
firmware = {'firmware_version': '0.94', 'checkout_sha': 'a'*40, 'manuals': manifest,
            'uf2_sha256': sha(files['build/CPokecombasic.uf2']), 'uf2_bytes': len(files['build/CPokecombasic.uf2']),
            'host_results': {'tests': 71, 'failures': 0, 'errors': 0, 'skipped': 0},
            'build_number': 'fixture', 'hardware_acceptance': 'PENDING'}
files['build/firmware-manifest.json'] = json.dumps(firmware).encode()
audit = {'source_sha': firmware['checkout_sha'], 'files':
         [{'path': name, 'sha256': sha(data)} for name, data in files.items()]}
files['build/package-audit.json'] = json.dumps(audit).encode()
assert module.validate(files)['pass']

def reject(bad):
    try: module.validate(bad)
    except (AssertionError, KeyError): pass
    else: raise AssertionError('invalid package accepted')

for name in ('inner.zip', '../escaped', '/absolute', 'back\\slash', 'build/debug.elf',
             'build/firmware.map', 'build/object.o', 'build/keyboard-i2c-recovery.uf2',
             'build/CPokecombasic-editor-perf.uf2', 'untracked.txt'):
    reject({**files, name: b'bad'})
for name in ('flash-cpb.cmd', 'build/CPokecombasic.uf2', 'docs/System.pdf', 'README.md'):
    reject({**files, name: b'corruption'})
reject({name: data for name, data in files.items() if name != 'flash-cpb.cmd'})
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)/'artifact.zip'
    with zipfile.ZipFile(path, 'w') as archive:
        for name, data in files.items(): archive.writestr(name, data)
    assert module.validate(module.read_package(path))['pass']
    # One extraction with the original relative layout, then revalidate all hashes.
    with zipfile.ZipFile(path) as archive: archive.extractall(Path(tmp)/'extracted')
    assert module.validate(module.read_package(Path(tmp)/'extracted'))['pass']
print('Package / original ZIP / one extraction / hashes / debug / nested ZIP rejection: PASS')

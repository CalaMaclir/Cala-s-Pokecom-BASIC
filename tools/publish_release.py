"""Audit the original accepted Actions package and publish without rebuilding it."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

from validate_release_package import validate, read_package

ROOT=Path(__file__).resolve().parents[1]
REPO='CalaMaclir/Cala-s-Pokecom-BASIC'
OUT=ROOT/'release-assets'
CONFIG=ROOT/'docs/release/current-publication.json'

def digest(data):return hashlib.sha256(data).hexdigest()

def git_blob(data):
    return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()

def get(url):
    with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'CPB-release-audit'}),timeout=90) as response:
        return response.read()

def api(method,path,body=None,content_type='application/json'):
    base='https://api.github.com/repos/'+REPO
    url=path if path.startswith('https://uploads.github.com/') else base+path
    headers={'Authorization':'Bearer '+os.environ['GH_TOKEN'],'Accept':'application/vnd.github+json',
             'User-Agent':'CPB-release-audit','X-GitHub-Api-Version':'2022-11-28','Content-Type':content_type}
    data=json.dumps(body).encode() if isinstance(body,dict) else body
    with urllib.request.urlopen(urllib.request.Request(url,data=data,headers=headers,method=method),timeout=90) as response:
        return json.load(response)

def source_audit(config):
    manifest=json.loads((ROOT/config['source_manifest']).read_text())
    assert manifest['development_source']==config['development_source'] and manifest['tag']==config['tag']
    assert not manifest['private_history_imported']
    files=manifest['files'];names=[x['path'] for x in files]
    assert len(names)==len(set(names))
    links=0
    for item in files:
        path=PurePosixPath(item['path'])
        assert not path.is_absolute() and '..' not in path.parts
        data=(ROOT/item['path']).read_bytes()
        assert git_blob(data)==item['public_blob'],('public source blob',item['path'])
        if not item['intentional_adaptation']:
            assert item['development_blob']==item['public_blob'],item['path']
        if path.suffix=='.md':
            text=data.decode()
            text=re.sub(r'^(```|~~~).*?^\1[^\n]*$','',text,flags=re.M|re.S)
            text=re.sub(r'`[^`\n]*`','',text)
            for target in re.findall(r'\]\(([^\s)]+)\)',text):
                target=urllib.parse.urlsplit(target)
                if target.scheme or target.netloc or not target.path:continue
                destination=(ROOT/item['path']).parent/urllib.parse.unquote(target.path)
                assert destination.resolve().is_relative_to(ROOT)
                assert destination.exists(),('public documentation link',item['path'],target.path)
                links+=1
    assert f'set(RMB_VERSION "{config["version"]}"' in (ROOT/'CMakeLists.txt').read_text()
    record=json.loads((ROOT/config['hardware_acceptance']).read_text())
    assert record['status']=='USER_ACCEPTED' and record['version']==config['version']
    from firmware_manifest import accepted_product_matches
    assert accepted_product_matches(ROOT,record),'product differs from owner-accepted candidate'
    print('Public source audit PASS:',len(files),'blobs,',links,'current local Markdown links')
    return len(files),links

def prepare(config):
    assert os.environ['GITHUB_REPOSITORY']==REPO
    files,links=source_audit(config)
    relative=PurePosixPath(config['input_zip'])
    assert not relative.is_absolute() and '..' not in relative.parts
    data=(ROOT/relative).read_bytes()
    assert len(data)==config['zip_bytes'] and digest(data)==config['zip_sha256'],'original Actions ZIP size/digest'
    OUT.mkdir(exist_ok=True)
    zip_name=config['artifact_name']+'.zip'
    (OUT/zip_name).write_bytes(data)
    package=read_package(OUT/zip_name)
    result=validate(package)
    with zipfile.ZipFile(OUT/zip_name) as z:assert z.testzip() is None,'ZIP CRC'
    fw=json.loads(package['build/firmware-manifest.json'])
    assert fw['checkout_sha']==fw['branch_head_sha']==config['development_source']
    assert fw['repository']=='CalaMaclir/RetroMiniBASIC-PicoCalc'
    assert fw['workflow_run_id']==str(config['run_id']) and fw['build_number']==str(config['build_number'])
    assert fw['artifact_name']==config['artifact_name'] and fw['full_validation']=='true'
    assert fw['board']=='pico2_w' and fw['pico_sdk']=='2.3.1'
    assert fw['host_results']=={'tests':72,'failures':0,'errors':0,'skipped':0}
    assert fw['local_numeric_codegen']=={'pass':True,'fma_count':0}
    assert fw['release_ready'] and not fw['release_candidate']
    acceptance=fw['hardware_acceptance_record']
    assert acceptance['product_match'] and acceptance['sdk_match'] and acceptance['accepted_source']==config['accepted_source']
    record=json.loads((ROOT/config['hardware_acceptance']).read_text())
    sdk=fw['pico_sdk_identity']
    assert sdk['verified'] and sdk['commit']==record['pico_sdk_commit'] and sdk['submodules']==record['pico_sdk_submodules']
    assert acceptance['publication_build_device_test']=='NOT_RUN'
    assert digest((ROOT/config['hardware_acceptance']).read_bytes())==acceptance['sha256']
    assert package[config['hardware_acceptance']]==(ROOT/config['hardware_acceptance']).read_bytes()
    perf=fw['performance_report']
    assert perf['correctness_status']=='PASS' and perf['performance_mode']=='off' and perf['performance_status']=='NOT_RUN'
    cases=list(ET.fromstring(package['build-host/test-results.xml']).iter('testcase'))
    assert len(cases)==72 and all(c.find(tag) is None for c in cases for tag in ('failure','error','skipped'))
    uf2=package['build/CPokecombasic.uf2']
    assert len(uf2)>0 and len(uf2)%512==0
    for offset in range(0,len(uf2),512):
        block=uf2[offset:offset+512]
        m1,m2,flags,address,size,number,total,family=struct.unpack('<8I',block[:32])
        assert (m1,m2)==(0x0A324655,0x9E5D5157) and struct.unpack('<I',block[-4:])[0]==0x0AB16F30
        assert 0<size<=476 and 0<=number<total
    name=f'Cala-Pokecom-BASIC-v{config["version"]}-pico2w.uf2'
    (OUT/name).write_bytes(uf2)
    names=[zip_name,name]
    manuals=json.loads(package['docs/manuals-manifest.json'])
    assert manuals==fw['manuals']==json.loads((ROOT/'docs/manuals-manifest.json').read_text())
    assert manuals['version']==config['version']
    for manual in manuals['manuals']:
        for field in ('source','output'):
            assert digest(package[manual[field]])==manual[field+'_sha256']
            assert (ROOT/manual[field]).read_bytes()==package[manual[field]]
        assert package[manual['output']].startswith(b'%PDF-')
        name=Path(manual['output']).name;(OUT/name).write_bytes(package[manual['output']]);names.append(name)
    for path,content in package.items():
        assert all(marker not in content for marker in (b'/home/',b'actions-runner',b'CPB-LOCAL')),('private marker',path)
    (OUT/'SHA256SUMS.txt').write_text(''.join(digest((OUT/name).read_bytes())+'  '+name+'\n' for name in names))
    names.append('SHA256SUMS.txt')
    public_sha=os.environ['GITHUB_SHA']
    body=(ROOT/f'docs/release/v{config["version"]}-release-notes.md').read_text()
    body=re.sub(r'\]\((v[^/()]+\.md)\)',lambda match:'](https://github.com/'+REPO+'/blob/'+public_sha+'/docs/release/'+match.group(1)+')',body)
    body+='\n\n## Release provenance\n\n'
    body+='- Public source commit: `'+public_sha+'`\n'
    body+='- Development source: `'+config['development_source']+'`\n'
    body+=f'- Verified FULL build: #{config["build_number"]} / run {config["run_id"]} / artifact {config["artifact_id"]}\n'
    body+='- Original ZIP SHA256: `'+config['zip_sha256']+'`\n'
    body+='- Owner-accepted candidate: `'+config['accepted_source']+'` / build #870, 2026-10-04. Publication product blobs and SDK match; no new publication-build device test.\n'
    body+='- Performance: current publication build off/NOT_RUN; accepted candidate build #870 FULL/report retains its timing WARN results. Product source unchanged.\n'
    body+='\nWindows one-click updater (flash-cpb.cmd) is included only in the complete original Actions artifact ZIP. The ZIP has not been recompressed.\n'
    body+='\nAssets: '+', '.join('`'+name+'`' for name in names)+'\n'
    (OUT/'release-body.md').write_text(body)
    receipt={'tag':config['tag'],'public_commit':public_sha,'development_source':config['development_source'],
        'source_files_verified':files,'current_links_verified':links,'build':config['build_number'],
        'run':config['run_id'],'artifact_id':config['artifact_id'],'accepted_candidate':config['accepted_source'],
        'assets':[{'name':name,'size':(OUT/name).stat().st_size,'sha256':digest((OUT/name).read_bytes())} for name in names],
        'zip_unchanged':True,'correctness':'PASS','package_audit':result,'host_tests':72,
        'device_smoke':'NOT_RUN for publication binary; owner-accepted candidate product source matched',
        'windows_flashing':'NOT_RUN','timing_status':'NOT_RUN; accepted candidate report reviewed separately'}
    (OUT/'publication-audit.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(receipt,ensure_ascii=False,indent=2))

def publish(config):
    assert os.environ['GITHUB_REPOSITORY']==REPO and os.environ['GITHUB_REF']=='refs/heads/main'
    receipt=json.loads((OUT/'publication-audit.json').read_text())
    assert receipt['public_commit']==os.environ['GITHUB_SHA']
    assert api('GET','/git/ref/heads/main')['object']['sha']==receipt['public_commit']
    source_audit(config)
    for item in receipt['assets']:assert digest((OUT/item['name']).read_bytes())==item['sha256']
    tag=config['tag']
    try:
        existing_tag=api('GET','/git/ref/tags/'+tag)['object']
    except urllib.error.HTTPError as error:
        if error.code!=404:raise
    else:
        if existing_tag['type']=='tag':existing_tag=api('GET','/git/tags/'+existing_tag['sha'])['object']
        assert existing_tag['type']=='commit' and existing_tag['sha']==receipt['public_commit'],'existing tag differs'
    try:release=api('GET','/releases/tags/'+tag)
    except urllib.error.HTTPError as error:
        if error.code!=404:raise
        release=api('POST','/releases',{'tag_name':tag,'target_commitish':receipt['public_commit'],
            'name':"Cala's Pokecom BASIC Version "+config['version'],
            'body':(OUT/'release-body.md').read_text(),'draft':True,'prerelease':False})
    expected={x['name']:x for x in receipt['assets']}
    if release['draft']:
        assert release['target_commitish']==receipt['public_commit']
        existing={a['name']:a for a in release['assets']}
        assert set(existing)<=set(expected),'unexpected draft assets'
        upload=release['upload_url'].split('{')[0]
        for name,item in expected.items():
            asset=existing.get(name)
            if asset is None:
                asset=api('POST',upload+'?'+urllib.parse.urlencode({'name':name}),(OUT/name).read_bytes(),'application/octet-stream')
            assert asset['size']==item['size'] and asset['digest']=='sha256:'+item['sha256'],('uploaded digest',name)
        current=api('GET','/releases/'+str(release['id']))
        assert {a['name'] for a in current['assets']}==set(expected)
        release=api('PATCH','/releases/'+str(release['id']),{'draft':False,'prerelease':False,'make_latest':'true'})
    assert not release['draft'] and not release['prerelease']
    ref=api('GET','/git/ref/tags/'+tag)['object']
    if ref['type']=='tag':ref=api('GET','/git/tags/'+ref['sha'])['object']
    assert ref['type']=='commit' and ref['sha']==receipt['public_commit']
    current=api('GET','/releases/'+str(release['id']))
    assert {a['name'] for a in current['assets']}==set(expected)
    for asset in current['assets']:
        item=expected[asset['name']];downloaded=get(asset['browser_download_url'])
        assert len(downloaded)==item['size'] and digest(downloaded)==item['sha256'],('public download',asset['name'])
    receipt.update(published=True,release_url=current['html_url'],release_id=current['id'],tag_commit=ref['sha'],public_download_hashes='PASS')
    (OUT/'publication-audit.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(receipt,ensure_ascii=False,indent=2))

if __name__=='__main__':
    config=json.loads(CONFIG.read_text())
    command=sys.argv[1]
    if command=='source-audit':source_audit(config)
    elif command=='prepare':prepare(config)
    elif command=='publish':publish(config)
    else:raise ValueError('source-audit, prepare, or publish required')

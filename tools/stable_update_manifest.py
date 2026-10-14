#!/usr/bin/env python3
"""Produce only stable.json from an existing, accepted public GitHub Release.

No firmware builds, release creation, asset uploads or tag mutations. Suitable
for the public repository's trusted post-publication metadata workflow.
"""
import argparse,base64,hashlib,json,os,re,urllib.request,urllib.error
from pathlib import Path
REPO='CalaMaclir/Cala-s-Pokecom-BASIC'

def sha(data):return hashlib.sha256(data).hexdigest()
def fetch(url,limit=16*1024*1024):
    request=urllib.request.Request(url,headers={'User-Agent':'CPB-stable-feed','Accept':'application/vnd.github+json'})
    with urllib.request.urlopen(request,timeout=90) as response:
        assert response.url.startswith('https://'),'HTTPS required'
        data=response.read(limit+1)
        assert len(data)<=limit,'response limit'
        return data

def api(path):return json.loads(fetch('https://api.github.com/repos/'+REPO+path))
def make(release,config,acceptance,public_sha,sums,uf2):
    assert release['draft'] is False and release['prerelease'] is False
    assert release['tag_name']==config['tag']
    assert release['html_url']=='https://github.com/'+REPO+'/releases/tag/'+config['tag']
    assert re.fullmatch(r'\d+\.\d+(?:\.\d+)?',config['version'])
    assert acceptance['status']=='USER_ACCEPTED' and acceptance['version']==config['version']
    assert acceptance['accepted_source']==config['accepted_source']
    assert re.fullmatch('[0-9a-f]{40}',config['development_source']) and re.fullmatch('[0-9a-f]{40}',public_sha)
    name=f'Cala-Pokecom-BASIC-v{config["version"]}-pico2w.uf2'
    assets={a['name']:a for a in release['assets']}
    assert len(assets)==len(release['assets']), 'duplicate asset names'
    assert name in assets and 'SHA256SUMS.txt' in assets
    entries={}
    for line in sums.decode('ascii').splitlines():
        match=re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_.-]+)',line)
        assert match and match[2] not in entries,'invalid checksum list'
        entries[match[2]]=match[1]
    assert 0<len(uf2)<=8*1024*1024 and len(uf2)%512==0
    assert uf2[:8]==bytes.fromhex('5546320a57515d9e')
    assert sha(uf2)==entries[name]
    for asset,data in [(assets[name],uf2),(assets['SHA256SUMS.txt'],sums)]:
        assert asset['size']==len(data) and asset['digest']=='sha256:'+sha(data)
        assert asset['browser_download_url']=='https://github.com/'+REPO+'/releases/download/'+config['tag']+'/'+asset['name']
    assert int(config['build_number'])>0
    return {'schema':'cpb-update-v1','channel':'stable','board':'pico2_w','version':config['version'],
        'build':int(config['build_number']),'prerelease':False,'hardware_acceptance':'USER_ACCEPTED_PRODUCT_SOURCE',
        'tag':config['tag'],'release_url':release['html_url'],'source_sha':config['development_source'],
        'public_source_sha':public_sha,'uf2':{'name':name,'bytes':len(uf2),'sha256':sha(uf2),'url':assets[name]['browser_download_url']}}

def publish(data):
    assert os.environ['GITHUB_REPOSITORY']==REPO and os.environ['GITHUB_REF']=='refs/heads/main'
    url='https://api.github.com/repos/'+REPO+'/contents/stable.json'
    headers={'Authorization':'Bearer '+os.environ['GH_TOKEN'],'User-Agent':'CPB-stable-feed','Accept':'application/vnd.github+json'}
    current=None
    try:
        with urllib.request.urlopen(urllib.request.Request(url+'?ref=main',headers=headers),timeout=90) as response:current=json.load(response)
    except urllib.error.HTTPError as error:
        if error.code!=404:raise
    if current:
        previous=json.loads(base64.b64decode(current['content']))
        if previous==data:return
        version=lambda s:tuple((list(map(int,s.split('.')))+[0,0])[:3])
        assert version(data['version'])>=version(previous['version']),'stable pointer rollback'
        if version(data['version'])==version(previous['version']):
            assert data['build']>=previous['build'],'stable build rollback'
    body={'message':'Update verified stable firmware manifest for '+data['tag'],'branch':'main',
          'content':base64.b64encode((json.dumps(data,indent=2)+'\n').encode()).decode()}
    if current:body['sha']=current['sha']
    request=urllib.request.Request(url,data=json.dumps(body).encode(),headers={**headers,'Content-Type':'application/json'},method='PUT')
    with urllib.request.urlopen(request,timeout=90) as response:assert response.status in (200,201)
    # Confirm committed contents through the API (raw CDN may be briefly cached).
    with urllib.request.urlopen(urllib.request.Request(url+'?ref=main',headers=headers),timeout=90) as response:
        assert json.loads(base64.b64decode(json.load(response)['content']))==data

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--publish',action='store_true');parser.add_argument('--output',type=Path,default=Path('build/stable.json'));args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    config=json.loads((root/'docs/release/current-publication.json').read_text())
    acceptance=json.loads((root/config['hardware_acceptance']).read_text())
    release=api('/releases/latest');assert release['tag_name']==config['tag']
    tag=api('/git/ref/tags/'+config['tag'])['object']
    if tag['type']=='tag':tag=api('/git/tags/'+tag['sha'])['object']
    assert tag['type']=='commit'
    assets={a['name']:a for a in release['assets']}
    name=f'Cala-Pokecom-BASIC-v{config["version"]}-pico2w.uf2'
    data=make(release,config,acceptance,tag['sha'],fetch(assets['SHA256SUMS.txt']['browser_download_url'],65536),fetch(assets[name]['browser_download_url'],8*1024*1024))
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(data,indent=2)+'\n')
    if args.publish:publish(data)
    print('Verified stable manifest:',data['tag'],data['uf2']['sha256'])
if __name__=='__main__':main()

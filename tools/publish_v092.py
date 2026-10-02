"""Audit and publish the authorized v0.92 distribution, without rebuilding firmware."""
import hashlib, json, os, re, struct, sys, urllib.error, urllib.parse, urllib.request, zipfile
from pathlib import Path
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[1]
REPO="CalaMaclir/Cala-s-Pokecom-BASIC"
TAG="v0.92.0"
SOURCE="1ee0477ca8b157e8e953cb4fecf48291705f3731"
ZIP_NAME="CPokecombasic-v0.92-build785-pico2w.zip"
ZIP_HASH="884a04641f6db51a02758d4dd5c9372a1913716c9c23dce75866a88c3d94e856"
ASSETS=[ZIP_NAME,"Cala-Pokecom-BASIC-v0.92-pico2w.uf2",
        "Cala-Pokecom-BASIC-v0.92-Install-Manual-ja.pdf",
        "Cala-Pokecom-BASIC-v0.92-System-Manual-ja.pdf",
        "Cala-Pokecom-BASIC-v0.92-Programming-Reference-ja.pdf","SHA256SUMS.txt"]
OUT=ROOT/"release-assets"
def digest(data):
    return hashlib.sha256(data).hexdigest()
def get(url):
    with urllib.request.urlopen(urllib.request.Request(url,headers={"User-Agent":"CPB-release-audit"}),timeout=90) as response:
        return response.read()
def api(method,path,body=None,content_type="application/json"):
    base="https://api.github.com/repos/"+REPO
    url=path if path.startswith("https://uploads.github.com/") else base+path
    headers={"Authorization":"Bearer "+os.environ["GH_TOKEN"],
             "Accept":"application/vnd.github+json","User-Agent":"CPB-release-audit",
             "X-GitHub-Api-Version":"2022-11-28","Content-Type":content_type}
    data=json.dumps(body).encode() if isinstance(body,dict) else body
    with urllib.request.urlopen(urllib.request.Request(url,data=data,headers=headers,method=method),timeout=90) as response:
        return json.load(response)
def source_audit():
    manifest=json.loads((ROOT/"docs/release/v0.92-public-source-manifest.json").read_text())
    assert manifest["development_source"]==SOURCE and manifest["tag"]==TAG
    for item in manifest["files"]:
        data=(ROOT/item["path"]).read_bytes()
        git_sha=hashlib.sha1(b"blob "+str(len(data)).encode()+b"\0"+data).hexdigest()
        assert git_sha==item["public_blob"],("public source blob",item["path"])
        if not item["intentional_adaptation"]:
            assert item["public_blob"]==item["development_blob"],item["path"]
    for path in ("README.md","README.en.md","docs/manual-ja.md"):
        for target in re.findall(r"\]\(([^)]+)\)",(ROOT/path).read_text()):
            if "://" in target or target.startswith("#"):continue
            target=target.split("#")[0]
            assert (ROOT/Path(path).parent/target).exists(),("public documentation link",path,target)
    assert 'set(RMB_VERSION "0.92"' in (ROOT/"CMakeLists.txt").read_text()
    return len(manifest["files"])
def prepare():
    assert os.environ["GITHUB_REPOSITORY"]==REPO
    request=json.loads((ROOT/"docs/release/v0.92-publication.json").read_text())
    assert request["tag"]==TAG and request["development_source"]==SOURCE
    assert request["zip_sha256"]==ZIP_HASH
    url=request["download_url"]
    # This scoped URL serves only the ZIP authorized for public distribution.
    assert urllib.parse.urlparse(url).scheme=="https"
    print("::add-mask::"+url,flush=True)
    data=get(url)
    assert len(data)==3424388 and digest(data)==ZIP_HASH,"original Actions ZIP digest/size"
    OUT.mkdir(exist_ok=True)
    zip_path=OUT/ZIP_NAME
    zip_path.write_bytes(data)
    files=source_audit()
    with zipfile.ZipFile(zip_path) as archive:
        assert archive.testzip() is None,"ZIP CRC"
        names=archive.namelist()
        assert len(names)==len(set(names)),"duplicate archive paths"
        assert all(not Path(n).is_absolute() and ".." not in Path(n).parts for n in names)
        assert "flash-cpb.cmd" in names and "build/CPokecombasic.uf2" in names
        firmware=json.loads(archive.read("build/firmware-manifest.json"))
        assert firmware["checkout_sha"]==SOURCE and firmware["branch_head_sha"]==SOURCE
        assert firmware["repository"]=="CalaMaclir/RetroMiniBASIC-PicoCalc"
        assert firmware["build_number"]=="785" and firmware["workflow_run_id"]=="36961465939"
        assert firmware["run_attempt"]=="1" and firmware["full_validation"]=="true"
        assert firmware["board"]=="pico2_w" and firmware["artifact_name"]==ZIP_NAME[:-4]
        assert firmware["host_results"]=={"tests":47,"failures":0,"errors":0,"skipped":0}
        assert firmware["local_numeric_codegen"]=={"pass":True,"fma_count":0}
        performance=firmware["performance_report"]
        assert performance["performance_mode"]=="off" and performance["performance_status"]=="NOT_RUN"
        assert archive.read("build/performance-status.json")
        audit=json.loads(archive.read("build/package-audit.json"))
        assert audit["source_sha"]==SOURCE and audit["uf2_unchanged"] and audit["elf_load_bytes_unchanged"]
        for item in audit["files"]:
            assert digest(archive.read(item["path"]))==item["sha256"],("package receipt",item["path"])
        cases=list(ET.fromstring(archive.read("build-host/test-results.xml")).iter("testcase"))
        assert len(cases)==47 and all(c.find(tag) is None for c in cases for tag in ("failure","error","skipped"))
        assert archive.read("build/memory-report.txt")
        uf2=archive.read("build/CPokecombasic.uf2")
        assert digest(uf2)==firmware["uf2_sha256"] and len(uf2)==firmware["uf2_bytes"]
        assert len(uf2)>0 and len(uf2)%512==0
        count=len(uf2)//512
        families={}
        for index in range(count):
            block=uf2[index*512:(index+1)*512]
            magic1,magic2,flags,address,size,number,total,family=struct.unpack("<8I",block[:32])
            assert (magic1,magic2)==(0x0A324655,0x9E5D5157)
            assert struct.unpack("<I",block[-4:])[0]==0x0AB16F30
            assert 0<=number<total and 0<size<=476,(index,flags,size,number,total,family)
            key=family if flags & 0x2000 else 0
            group=families.setdefault(key,{'total':total,'numbers':[]})
            assert group['total']==total,(family,total,group['total'])
            group['numbers'].append(number)
        # UF2 permits concatenated images for different family IDs.
        for family,group in families.items():
            assert sorted(group['numbers'])==list(range(group['total'])),(family,group['total'],len(group['numbers']))
        print('UF2 family images:', {hex(k):v['total'] for k,v in families.items()})
        (OUT/ASSETS[1]).write_bytes(uf2)
        manuals=json.loads(archive.read("docs/manuals-manifest.json"))
        assert manuals==firmware["manuals"] and manuals["version"]=="0.92"
        assert manuals==json.loads((ROOT/"docs/manuals-manifest.json").read_text())
        for manual,pages in zip(manuals["manuals"],(6,14,16),strict=True):
            for field,key in (("source","source_sha256"),("output","output_sha256")):
                content=archive.read(manual[field])
                assert digest(content)==manual[key]
                assert digest((ROOT/manual[field]).read_bytes())==manual[key]
            pdf=archive.read(manual["output"])
            assert pdf.startswith(b"%PDF-") and len(re.findall(rb"/Type\s*/Page\b",pdf))==pages
            (OUT/Path(manual["output"]).name).write_bytes(pdf)
        for name in names:
            content=archive.read(name)
            for forbidden in (b"/home/",b"actions-runner",b"CPB-LOCAL"):
                assert forbidden not in content,("private path",name,forbidden.decode())
    sums="".join(digest((OUT/name).read_bytes())+"  "+name+"\n" for name in ASSETS[:-1])
    (OUT/"SHA256SUMS.txt").write_text(sums)
    public_sha=os.environ["GITHUB_SHA"]
    body=(ROOT/"docs/release/v0.92-release-notes.md").read_text()
    body+="\n\nPublic source commit: "+public_sha+"\nVerified development source: "+SOURCE
    body+="\nVerified build: #785 / run36961465939 / artifact11208820156 / FULL/off.\n"
    body+="\nWindows one-click updater (flash-cpb.cmd) is included only in the complete build artifact ZIP.\n"
    body+="\nComplete package ZIP is the original GitHub Actions artifact; it has not been recompressed.\n"
    (OUT/"release-body.md").write_text(body)
    receipt={"tag":TAG,"public_commit":public_sha,"development_source":SOURCE,
             "source_files_verified":files,"build":785,"run":36961465939,
             "assets":[{"name":name,"size":(OUT/name).stat().st_size,
                        "sha256":digest((OUT/name).read_bytes())} for name in ASSETS],
             "zip_unchanged":True,"correctness":"PASS","host_tests":47,
             "timing_status":"NOT_RUN","device_smoke":"not newly executed during publication"}
    (OUT/"publication-audit.json").write_text(json.dumps(receipt,indent=2)+"\n")
    print(json.dumps(receipt,indent=2))
    print("Public source and original ZIP audit PASS")
def publish():
    assert os.environ["GITHUB_REPOSITORY"]==REPO
    assert os.environ["GITHUB_REF"]=="refs/heads/main"
    receipt=json.loads((OUT/"publication-audit.json").read_text())
    assert receipt["public_commit"]==os.environ["GITHUB_SHA"]
    assert api("GET","/git/ref/heads/main")["object"]["sha"]==receipt["public_commit"]
    for item in receipt["assets"]:
        assert digest((OUT/item["name"]).read_bytes())==item["sha256"]
    try:
        release=api("GET","/releases/tags/"+TAG)
    except urllib.error.HTTPError as error:
        if error.code!=404:raise
        release=api("POST","/releases",{"tag_name":TAG,
            "target_commitish":receipt["public_commit"],"name":"Cala's Pokecom BASIC Version 0.92",
            "body":(OUT/"release-body.md").read_text(),"draft":True,"prerelease":False})
    if release["draft"]:
        assert release["target_commitish"]==receipt["public_commit"]
        existing={a["name"]:a for a in release["assets"]}
        assert set(existing)<=set(ASSETS),"unexpected draft assets"
        upload=release["upload_url"].split("{")[0]
        for item in receipt["assets"]:
            name=item["name"]
            asset=existing.get(name)
            if asset is None:
                asset=api("POST",upload+"?"+urllib.parse.urlencode({"name":name}),
                          (OUT/name).read_bytes(),"application/octet-stream")
            assert asset["size"]==item["size"]
            assert asset["digest"]=="sha256:"+item["sha256"],("uploaded digest",name)
        current=api("GET","/releases/"+str(release["id"]))
        assert {a["name"] for a in current["assets"]}==set(ASSETS)
        release=api("PATCH","/releases/"+str(release["id"]),{"draft":False,"prerelease":False,"make_latest":"true"})
    assert not release["draft"] and not release["prerelease"]
    tag=api("GET","/git/ref/tags/"+TAG)["object"]
    if tag["type"]=="tag":
        tag=api("GET","/git/tags/"+tag["sha"])["object"]
    assert tag["type"]=="commit" and tag["sha"]==receipt["public_commit"]
    current=api("GET","/releases/"+str(release["id"]))
    assert {a["name"] for a in current["assets"]}==set(ASSETS)
    for item in receipt["assets"]:
        asset=next(a for a in current["assets"] if a["name"]==item["name"])
        downloaded=get(asset["browser_download_url"])
        assert digest(downloaded)==item["sha256"] and len(downloaded)==item["size"],("public download",item["name"])
    receipt.update({"published":True,"release_url":current["html_url"],"release_id":current["id"],
                    "tag_commit":tag["sha"],"public_download_hashes":"PASS"})
    (OUT/"publication-audit.json").write_text(json.dumps(receipt,indent=2)+"\n")
    print(json.dumps(receipt,indent=2))
    print("Formal v0.92.0 release published and all public downloads verified")
if __name__=="__main__":
    if sys.argv[1]=="prepare":prepare()
    elif sys.argv[1]=="publish":publish()
    else:raise ValueError("prepare or publish is required")

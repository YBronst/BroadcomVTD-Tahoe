#!/usr/bin/env python3
"""Read-only public identity/source/tag/release checks; never builds or publishes."""
import argparse,hashlib,json,os,pathlib,plistlib,struct,sys,uuid,zipfile
if sys.flags.optimize:
    raise SystemExit('Python optimization disables validation assertions; run without -O/PYTHONOPTIMIZE')
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--require-tag',action='store_true',help='require the exact release tag ref, including for manual workflow dispatch')
a=p.parse_args()
R=pathlib.Path(__file__).resolve().parents[1]
I=json.loads((R/'Release/identity.json').read_text())
assert I['project']=='BroadcomVTD-Tahoe' and I['kext']=='BroadcomVTD.kext'
assert I['version']=='0.2.17' and I['tag']=='v'+I['version']
if a.require_tag:
    assert os.environ.get('GITHUB_REF_TYPE')=='tag','--require-tag requires GITHUB_REF_TYPE=tag'
if a.require_tag or os.environ.get('GITHUB_REF_TYPE')=='tag':
    assert os.environ.get('GITHUB_REF_NAME')==I['tag'],'Tag/version mismatch; no automatic bump permitted'
def macho(blob):
    assert struct.unpack_from('<I',blob)[0]==0xfeedfacf
    assert struct.unpack_from('<I',blob,4)[0]==0x01000007,'Not x86_64'
    assert struct.unpack_from('<I',blob,12)[0]==11,'Not a kext bundle'
    offset=32;found=None
    for _ in range(struct.unpack_from('<I',blob,16)[0]):
        cmd,size=struct.unpack_from('<II',blob,offset)
        assert size>=8 and offset+size<=len(blob)
        if cmd==0x1b:found=str(uuid.UUID(bytes=blob[offset+8:offset+24])).upper()
        offset+=size
    assert found==I['uuid'],'UUID mismatch'
def bundle(read):
    info=plistlib.loads(read('Contents/Info.plist'))
    assert info['CFBundleIdentifier']==I['identifier']=='local.kgp.BroadcomVTD'
    assert info['CFBundleVersion']==info['CFBundleShortVersionString']==I['version']
    assert info['CFBundleExecutable']=='BroadcomVTD' and info['OSBundleLibraries']['as.vit9696.Lilu']=='1.7.2'
    for name,sha in I['bundle_files'].items():assert hashlib.sha256(read(name)).hexdigest()==sha,name
    binary=read('Contents/MacOS/BroadcomVTD')
    assert len(binary)==I['executable_bytes']==147408
    assert hashlib.sha256(binary).hexdigest()==I['executable_sha256']
    macho(binary)
bundle(lambda name:(R/'Release/BroadcomVTD.kext'/name).read_bytes())
archive_digest,archive_name=(R/'Release/SHA256SUMS').read_text().split()
assert archive_name==I['project']+'-'+I['tag']+'.zip'
assert hashlib.sha256((R/'Release'/archive_name).read_bytes()).hexdigest()==archive_digest
packaged_files={
    'INSTALL.md':'Release/INSTALL.md',
    'RELEASE_NOTES_v'+I['version']+'.md':'RELEASE_NOTES_v'+I['version']+'.md',
    'LICENSE':'LICENSE',
    'THIRD_PARTY_NOTICES.md':'THIRD_PARTY_NOTICES.md',
    'Licenses/Lilu-BSD-3-Clause.txt':'Licenses/Lilu-BSD-3-Clause.txt',
    'Licenses/MacKernelSDK-APSL-2.0.txt':'Licenses/MacKernelSDK-APSL-2.0.txt',
    'identity.json':'Release/identity.json',
}
with zipfile.ZipFile(R/'Release'/archive_name) as z:
    assert len(z.namelist())==len(set(z.namelist())),'Duplicate ZIP member names'
    expected=set(packaged_files)|{'BroadcomVTD.kext/'+name for name in I['bundle_files']}
    assert {entry.filename for entry in z.infolist() if not entry.is_dir()}==expected,'Unexpected or missing release ZIP files'
    assert z.testzip() is None
    bundle(lambda name:z.read('BroadcomVTD.kext/'+name))
    stale=[name for name,source in packaged_files.items() if z.read(name)!=(R/source).read_bytes()]
    assert not stale,'Stale packaged repository files: '+', '.join(stale)
for name,sha in json.loads((R/'Config/frozen-source-sha256.json').read_text()).items():
    assert hashlib.sha256((R/name).read_bytes()).hexdigest()==sha,'Frozen source divergence: '+name
info=plistlib.loads((R/'POC/Info.plist').read_bytes())
assert info['CFBundleVersion']==info['CFBundleShortVersionString']=='0.2.25'
assert '-DMODULE_VERSION=0.2.25' in (R/'Makefile').read_text()
for name in ['README.md','RELEASE_NOTES_v0.2.25.md','Docs/PHYSICAL_VALIDATION_v0.2.17.md']:
    text=(R/name).read_text();assert ('0.2.25' in text or '0.2.17' in text) and 'EXPERIMENTAL' in text
    assert 'OCLP-CustoMac 3.0.3' in text and 'Modern Wireless' in text
print('PASS exact frozen source, bundled release, archive, metadata and tag identity')

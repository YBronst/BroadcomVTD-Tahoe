#!/usr/bin/env python3
"""Static inspection only; no kext loading, installation or live driver calls."""
import hashlib
import json
import pathlib
import plistlib
import re
import struct
import subprocess
import sys
import zipfile

if sys.flags.optimize:
    raise SystemExit('Python optimization disables validation assertions; run without -O/PYTHONOPTIMIZE')

ROOT=pathlib.Path(__file__).resolve().parents[1]
TC=pathlib.Path(subprocess.check_output(['xcrun','--find','clang++'],text=True).strip()).parent
BUILD=ROOT/'build/poc-0.2.25'
KEXT=BUILD/'BroadcomVTD.kext'
BIN=KEXT/'Contents/MacOS/BroadcomVTD'

def symbols(path=None, blob=None):
    text=subprocess.check_output([str(TC/'llvm-nm'),str(path) if path else '-'],input=blob).decode()
    defined=set(); undefined=set()
    for l in text.splitlines():
        f=l.split()
        if len(f)==3 and f[1].upper()!='U': defined.add(f[2])
        elif len(f)>=5 and f[0]=='I' and f[2]=='(indirect' and f[3]=='for':
            # Tahoe KPI stubs redirect allocator exports to *_external. These
            # are actual exported aliases, not undefined/undeclared symbols.
            defined.add(f[1])
        elif len(f)==2 and f[0]=='U': undefined.add(f[1])
    return defined,undefined

info=plistlib.loads((KEXT/'Contents/Info.plist').read_bytes())
assert info['CFBundleIdentifier']=='local.kgp.BroadcomVTD'
assert info['CFBundleVersion']==info['CFBundleShortVersionString']=='0.2.25'
assert info['OSBundleLibraries']['as.vit9696.Lilu']=='1.7.2'
subprocess.run(['/usr/bin/codesign','--verify','--strict',str(KEXT)],check=True)
signature_output=subprocess.run(['/usr/bin/codesign','--display','--verbose=4',str(KEXT)],
                                text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=True).stdout
assert re.findall(r'^Signature=(.*)$',signature_output,re.M)==['adhoc'],'Built kext is not explicitly ad-hoc signed'
assert re.findall(r'^TeamIdentifier=(.*)$',signature_output,re.M)==['not set'],'Built kext has a signing team or missing team metadata'
file=subprocess.check_output(['/usr/bin/file',str(BIN)]).decode().strip()
assert 'x86_64' in file and 'kext bundle' in file
data=BIN.read_bytes()
assert bytes.fromhex('e4678feb1dc135cc930648021083d04a') in data
_,imports=symbols(BIN)
nm_text=subprocess.check_output([str(TC/'llvm-nm'),str(BIN)]).decode()
addresses={f[2]:int(f[0],16) for line in nm_text.splitlines()
           if len(f:=line.split())==3}
# Read the compiled ABI-role table, not merely source strings.
table=addresses['__ZN3bvpL18ownershipReadSitesE'];offset=32;sites=None
for _ in range(struct.unpack_from('<I',data,16)[0]):
    command,size=struct.unpack_from('<II',data,offset)
    if command==0x19:
        va,vs,fo,fs=struct.unpack_from('<QQQQ',data,offset+24)
        if va<=table and table+7*16<=va+fs:
            sites=[struct.unpack_from('<QI',data,fo+table-va+i*16) for i in range(7)]
    offset+=size
assert sites==[(0x2c2fb8,1),(0x2c2ff6,1),(0x2c4245,2),(0x2c1fe9,3),(0x2c201a,3),(0x2c37b9,4),(0x2c37e5,4)]
original_base=addresses['__ZN3bvpL9originalsE']
gate_text=(ROOT/'Config/TargetGate.hpp').read_text()
hooks=[s.strip() for s in re.search(r'enum Hook \{ (.+), HookCount \}',gate_text)[1].split(',')]
route_proof={}
disassemblies=[]
for index,name in enumerate(hooks):
    candidates=[s for s in addresses if re.match(r'__ZN3bvpL\d+wrap'+name+r'E',s)]
    assert len(candidates)==1,(name,candidates)
    dis=subprocess.check_output([str(TC/'llvm-objdump'),'--macho','--disassemble',
                                '--dis-symname',candidates[0],str(BIN)]).decode()
    disassemblies.append(dis)
    hits=[]
    # Decode RIP-relative trampoline call/jump/load bytes, not display labels.
    # Clang can split the one source call into conditional call/tail-call sites.
    for m in re.finditer(r'^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.+)$',dis,re.M):
        raw=bytes.fromhex(m[2])
        indirect=len(raw)==6 and raw[:2] in (b'\xff\x15',b'\xff\x25')
        load=len(raw)==7 and raw[0] in (0x48,0x4c) and raw[1]==0x8b and raw[2]&0xc7==5
        if not (indirect or load): continue
        address=int(m[1],16)+len(raw)+struct.unpack('<i',raw[-4:])[0]
        if address==original_base+index*8: hits.append({'address':m[1],'instruction':m[3]})
    assert hits,(name,'no compiled original-slot reference')
    route_proof[name]={'symbol':candidates[0],'original_slot':hex(original_base+index*8),
                      'compiled_original_references':hits}
(BUILD/'abi').mkdir(exist_ok=True)
(BUILD/'abi/compiled-probe-wrappers.asm').write_text('\n'.join(disassemblies))
(BUILD/'compiled-route-validation.json').write_text(json.dumps(route_proof,indent=2)+'\n')
bind=next(s for s in addresses if re.match(r'__ZN3bvpL\d+bindTargetE',s))
bind_dis=subprocess.check_output([str(TC/'llvm-objdump'),'--macho','--disassemble',
                                 '--dis-symname',bind,str(BIN)]).decode()
pcisym=next(s for s in addresses if re.match(r'__ZN3bvpL\d+exactPCIE',s))
pci_dis=subprocess.check_output([str(TC/'llvm-objdump'),'--macho','--disassemble',
                                '--dis-symname',pcisym,str(BIN)]).decode()
assert '$0x14e4' in pci_dis and '$0x43ba' not in pci_dis
assert '$0x28000' in pci_dis
assert 'controllerVtable' in bind_dis
(BUILD/'abi/compiled-target-acquisition.asm').write_text(bind_dis+'\n'+pci_dis)
# Public CI checks imports against the frozen tested executable's import inventory.
# It does not claim a live Tahoe kernel/provider symbol-resolution test.
allowed=set((ROOT/'Config/frozen-imports.txt').read_text().splitlines())
missing=sorted(imports-allowed)
outside_declared=missing
forbidden=[s for s in imports if any(t in s for t in ('configWrite','IODMACommand','IOMbuf','mbuf_freem','mbuf_free','IOSleep'))]
assert not forbidden,forbidden
result=dict(bundle_identifier=info['CFBundleIdentifier'],version=info['CFBundleVersion'],
            signature=dict(strict_verification=True,signature='adhoc',team_identifier='not set',
                           codesign_display_output=signature_output),
            file=file,binary_size=len(data),binary_sha256=hashlib.sha256(data).hexdigest(),
            imports=len(imports),imports_outside_frozen_inventory=missing,
            compiled_original_slots_verified=len(route_proof),
            compiled_seven_ownership_read_sites=sites,
            broadcom_vendor_class_gate_no_corrective_device_whitelist=True,
            exact_controller_vtable_in_compiled_binding=True,
            live_provider_linkage_checked=False,
            forbidden_unrelated_control_imports=forbidden,
            symbol_provider_caveat='Frozen import inventory comparison only; CI does not supply a live Tahoe kernel/provider linkage test.')
assert any('copyMapperForDevice' in x for x in imports)
assert '__ZN8IOMapper7gSystemE' in imports
assert b'iommu-parent' in data
relocs=subprocess.check_output(['/usr/bin/otool','-rv',str(BIN)]).decode()
system_relocations=[int(line.split()[0],16) for line in relocs.splitlines()
                    if line.endswith('__ZN8IOMapper7gSystemE')]
assert len(system_relocations)==1
system_loads=[]
for match in re.finditer(r'^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.+)$',bind_dis,re.M):
    raw=bytes.fromhex(match[2])
    if len(raw)==7 and raw[0] in (0x48,0x4c) and raw[1]==0x8b and raw[2]&0xc7==5:
        target=int(match[1],16)+7+struct.unpack('<i',raw[-4:])[0]
        if target==system_relocations[0]:
            system_loads.append(dict(address=match[1],instruction=match[3]))
assert system_loads, 'No relocation-backed gSystem read in compiled admission'
result['system_mapper_import_relocation']=hex(system_relocations[0])
result['compiled_system_mapper_loads']=system_loads
assert any('withOptions' in x and 'IOMemoryDescriptor' in x for x in imports)
assert b'AppleVTD' in data
for obsolete in [b'-brcmvtd\x00',b'-brcmvtdrx\x00',b'-brcmvtdtxchain\x00',b'-brcmvtdprivatetx300\x00',b'-brcmvtdnotxcleanup\x00']:
    assert obsolete not in data,obsolete
result['explicit_mapper_and_prepared_md_imports']=True
result['verified_system_applevtd_selection_encoded']=True
result['zero_required_positive_boot_arguments']=True
result['runtime_modes']=['NATIVE_PASSTHROUGH','APPLEVTD_CORRECTIVE_EXPERIMENTAL']
result['rev49_terminal_contract']='C / informal C+; experimental 300-us predicate, not target hardware proof'
(BUILD/'static-validation.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
if missing or outside_declared: raise SystemExit(1)

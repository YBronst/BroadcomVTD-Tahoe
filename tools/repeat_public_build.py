#!/usr/bin/env python3
"""Repeat only CI builds; never writes Release/ or claims physical validation."""
import hashlib,json,pathlib,subprocess,sys
if sys.flags.optimize:
    raise SystemExit('Python optimization disables validation assertions; run without -O/PYTHONOPTIMIZE')
R=pathlib.Path(__file__).resolve().parents[1];O=R/'build/poc-0.2.25';O.mkdir(parents=True,exist_ok=True)
def inventory(root):
    return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob('*')) if p.is_file()}
release=inventory(R/'Release');first=None
with (O/'repeat-build.log').open('w') as log:
    for n in range(2):
        subprocess.run(['make','-B','-j2','all'],cwd=R,stdout=log,stderr=subprocess.STDOUT,check=True)
        current=inventory(O/'BroadcomVTD.kext')
        if first is None:first=current
        else:assert current==first,'Same-environment bundle mismatch'
assert inventory(R/'Release')==release,'Official release changed'
(O/'repeat-validation.json').write_text(json.dumps({'byte_identical':True,'scope':'two same-environment CI builds; NOT PHYSICALLY VALIDATED','files':first},indent=2)+'\n')
print('PASS repeated CI bundles byte-identical; official release untouched')

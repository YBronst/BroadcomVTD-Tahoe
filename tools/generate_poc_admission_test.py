#!/usr/bin/env python3
"""Host harness compiles the REAL frontend bodies, not a reservation/drop model."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
src=(ROOT/'POC/Frontend.cpp').read_text()
parts=[]
for start,end in [('static privateTx::Geometry privateGeometry(','struct TxResetObservation'),
                  ('static Event ringEvent(', 'static void rememberRing('),
                  ('static int32_t wrapTx(', 'static int32_t wrapUnframed(')]:
    assert src.count(start)==1 and src.count(end)==1
    parts.append(src[src.index(start):src.index(end)])
out=ROOT/'build/poc-0.2.25/generated/FrontendAdmission.inc'
out.parent.mkdir(parents=True,exist_ok=True)
out.write_text('// Generated exact Frontend.cpp slices; do not edit.\n'+'\n'.join(parts))
print('Host harness: exact TxLease, ringEvent and wrapTx source bodies extracted.')

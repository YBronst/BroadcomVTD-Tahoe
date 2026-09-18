#!/usr/bin/env python3
"""Extract exact active frontend functions; only native APIs are host doubles."""
from pathlib import Path
R=Path(__file__).resolve().parents[1]
s=(R/'POC/Frontend.cpp').read_text()
parts=[]
for a,b in [('static void configureCorrection(','static bool bindTarget('),
 ('static privateTx::Geometry privateGeometry(','// RX observation remains passive'),
 ('static Event ringEvent(', 'static void rememberRing('),
 ('static int32_t wrapTx(', 'static int32_t wrapUnframed('),
 ('static void *wrapReclaim(', 'static void wrapSuspend('),
 ('static bool wrapReset(', '// BVTQ-BEGIN: new route observes'),
 ('static void wrapTxInit(', 'static uint32_t wrapReadReg(')]:
    assert s.count(a)==s.count(b)==1
    parts.append(s[s.index(a):s.index(b)])
p=R/'build/poc-0.2.25/generated/FrontendPrivate.inc';p.parent.mkdir(parents=True,exist_ok=True)
p.write_text('// Exact current Frontend.cpp bodies; no behavioral substitution.\n'+'\n'.join(parts))
print('PASS actual private TX/reclaim/free/reset/init frontend extracted')

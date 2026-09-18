#!/usr/bin/env python3
"""Compile actual reclaim frontend under mocked native-call/return-PC inputs."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
src=(ROOT/'POC/Frontend.cpp').read_text()
parts=[]
for start,end in [('static privateTx::Geometry privateGeometry(','struct TxResetObservation'),
                  ('static Event ringEvent(', 'static void rememberRing('),
                  ('static void *wrapReclaim(', 'static void wrapFree(')]:
    assert src.count(start)==src.count(end)==1
    parts.append(src[src.index(start):src.index(end)])
dest=ROOT/'build/poc-0.2.25/generated/FrontendQualification.inc'
dest.parent.mkdir(parents=True,exist_ok=True)
dest.write_text('// Exact frontend slices; caller PC/native getter supplied by host mocks.\n'+'\n'.join(parts))
print('Host harness: exact TxLease, ringEvent and wrapReclaim extracted.')

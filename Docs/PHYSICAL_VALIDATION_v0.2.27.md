# BroadcomVTD 0.2.27 — physical validation PASS

Physically validated experimental release, frozen after independent source
review and controlled KGP testing. This is a public adaptation of the frozen
report identified in [provenance](PROVENANCE.md), not a new hardware campaign.

## Validated environment and evidence levels

ASUS WS X299 Sage/10G; BCM943602CDP, PCI 14e4:43ba, D11 rev49; macOS Tahoe 26.6.2 (25G83);
**OCLP-CustoMac 3.0.3 with Modern Wireless root patches**; AppleVTD enabled,
`DisableIoMapper=false`; exact BroadcomVTD 0.2.17 release. OCLP-CustoMac restores
Modern Wireless; this independent plugin addresses the separate DMA/IOMMU layer.

Project scope: **macOS Tahoe / Darwin 25.x**.

- SOURCE-PROVEN: reviewed implementation/native-control-flow findings.
- BUILD-PROVEN: host/sanitizer/compiled validation and binary identity—not hardware proof.
- TRACE-PROVEN: independently replayed saved raw data matching its JSONL.
- RUNTIME-PROVEN: demonstrated implementation behavior on the physical target.
- USER-OBSERVED: KGP's physical actions, interface and functional results.
- INFERENCE / UNKNOWN: interpretations, unestablished causality and absent evidence.

## Exact tested executable

`BroadcomVTD.kext`, `local.kgp.BroadcomVTD`, version 0.2.17, x86_64,
147408 bytes, SHA-256
`2a8f9641a9c9856b1e45899f31332a68ad2cf519e94951ca128bb3f262c3c349`,
UUID `D0B214B8-F096-3BC6-99AD-F1E9D8E788B6`.
The public release retains this exact bundle; no CI artifact substitutes for it.

## Fresh boot and Sleep/Wake

RUNTIME-PROVEN: automatic `APPLEVTD_CORRECTIVE_EXPERIMENTAL` selection,
TX used/quarantined 0/0, halt 0, pending OLD 0, RX enabled/unhalted and live 240.
USER-OBSERVED: zero required positive BroadcomVTD arguments, `en2` and `awdl0`
UP/RUNNING/active.

After exactly one Sleep/Wake, six selected bulk-reset tickets **27–32** record
caller **0xfb7e3**, observed revision **49**, and
`caller_is_ownership_authority=false`. SOURCE-PROVEN: this caller is the
`_wlc_bmac_corereset` six-ring reset family. It is telemetry, not a caller release
whitelist. Baseline ticket 20 had the other native family caller 0x10716c with
the same authority=false semantics.

The exact active OLD transaction was **serial 1583 / generation 54 / ticket 30**.

| Sequence | Observed event |
| ---: | --- |
| 49144 | Reset ticket selected; OLD generation 54, new generation 62, serial fence 1583 |
| 49145 | Serial 1583 reset quarantine, reason 15 |
| 49146–49148 | Populated ring reset; valid reset-status read 0x8000; original true and disabled acknowledged |
| 49149–49150 | EXPERIMENTAL_300us_drain_begin/end, ticket 30 |
| 49151 | Stage 17: EXPERIMENTAL_active_association_ended_at_reset_terminal |
| 49152 | Stage 13: EXPERIMENTAL_old_DMA_retired_credit_returned; cumulative retired count 5 → 6 |
| 49409–49411 | Same packet returned through native range-1 reclaim, then native send-side FreeEnter/FreeExit |

The serial, generation, ticket, backing, ring, owner and IOVA identities match in
the retained source evidence; live kernel addresses are omitted from this public
adaptation. The active association is zero at the terminal records. No retained
reason-15 Mapping remains afterward. MD/slot-lease retirement does not free the
native packet; native software disposition follows its original path.

Stage 17 → Stage 13 timestamp spacing is **5669 ns / 5.669 us**, **not the
300-us drain**. Drain records precede both; their timestamp spacing is 391103 ns,
including overhead. Stage 17's event value is prepared before MD release but
emitted only after successful release. None of these timings is hardware proof.

## Repeated-cycle checkpoint

KGP reported approximately 5–6 Sleep attempts in that boot. Two woke early;
reset counters are not counts of completed user Sleep cycles. Latest retained
bulk tickets 155–160 again show 0xfb7e3 / rev49 / authority false.
Serial 5748 / generation 436 / ticket 158 reaches Stage 17 at 138619 and
Stage 13 at 138620. Cumulative retirement telemetry later reaches **46**.

RUNTIME-PROVEN end of the multi-cycle campaign:
TX used/quarantined **0/0**, halt **0**, pending OLD **0**, NoCredit **0**;
RX enabled/unhalted, reset-completed **2640**, live **240**.
The earlier per-cycle retained-credit accumulation did not recur in this campaign.
One retained Stage 17 and 25 retained Stage 13 records are **window counts**,
not boot-global event counts.

## Wireless validation and definitive final state

USER-OBSERVED: one earlier MBP → Hack AirDrop send failed after multi-Sleep
stress. Causality is UNKNOWN. KGP rebooted with the unchanged binary;
bidirectional AirDrop passed. After one controlled Sleep/Wake, Wi-Fi, AirPlay,
Screen Mirroring, tested AWDL/Continuity functionality including Continuity
Camera, bidirectional AirDrop and Personal Hotspot passed, bidirectionally where
applicable. **Ethernet was disabled for Wi-Fi and Personal Hotspot validation.**
The earlier AirDrop issue was not reproduced. Two early-wake observations were
separately associated in KGP's pmset evidence with HID/UserActivity; neither
currently establishes a BroadcomVTD regression.

The later post-all-tests capture is the definitive final mapper snapshot:

| Metric | Final result |
| --- | ---: |
| Gate / mode | provider bound / APPLEVTD_CORRECTIVE_EXPERIMENTAL |
| TX used / quarantined / mapping entries | **0 / 0 / 0** |
| TX halt / pending OLD | **0 / 0** |
| Historical quarantine events | 206 |
| Cumulative NoCredit | 1071 |
| Reservations / submissions after pressure | 23748 / 23748 |
| Normal completions after pressure | 23650 |
| RX prepared / mapped | 38620 / 38620 |
| RX completed / reset completed | 37660 / 720 |
| RX live | **240** |
| First permanent failure | not captured; stable, enabled retention; reason 0 |

`38620 - 37660 - 720 = 240`. RX is enabled and not halted.
**206 quarantine events and 1071 NoCredit events are cumulative history, not
current retained occupancy or a final fault.** Later admission/traffic and the
empty final snapshot demonstrate recovery; every individual pressure event was
not separately paired with a completion.

This capture has different provider/controller identities after the reported
reboot. Do not subtract its counters from BOOT-A's counters. The earlier
NoCredit=0 and later NoCredit=1071 are both correct for their capture/boot.

## Coverage and identities

TRACE-PROVEN: All four BIN files are 6771888 bytes, format 13 with 200-byte records. Every
raw header and retained event was replayed against the saved JSONL successfully.
Each retains 32768 events, zero missing inside the retained window, zero
busy/table drops, and no captured first permanent failure. Footers are non-atomic.

| Capture ID | Retained sequences |
| --- | --- |
| BOOT-A-BASELINE | 8407–41174 |
| BOOT-A-POST-SLEEP-WAKE-1 | 29182–61949 |
| BOOT-A-POST-MULTI-SLEEP-WAKE | 137514–170281 |
| FINAL-POST-ALL-TESTS | 499399–532166 |

The final window has zero Stage 17/13 records; prior terminal events have rolled
out, not been disproved. Other final-window private records carry cumulative
retirement value 206. No newly conclusive executed same-pointer requeue is
claimed; pointer reuse alone would not prove it. Native-passthrough mode was
not newly physically tested. The raw traces remain private, identified here:

| Capture | BIN SHA-256 | JSONL SHA-256 |
| --- | --- | --- |
| BOOT-A-BASELINE | `c253ed66a09683824c6f0e06a7dbe3d9341f521c86d6c04b3ffc3a6c8a9e4b06` | `b5090692bb1a26016d00bc6c06da2874f8a07bfbe4368b3d539314bda91223d6` |
| BOOT-A-POST-SLEEP-WAKE-1 | `8a52858b02340496e40126df6c83dc093d5e10f207bb3029224321c5f1421ae5` | `4f589e8b87a44b5f2fbc402b92d849ef9cc300ef11e61dfe3da63e179f042f63` |
| BOOT-A-POST-MULTI-SLEEP-WAKE | `981876d3de2f3a510a85d137f8b4cf4786bd849f301244ae43e800669a4ac28e` | `ba849c5dc8bd7bec93b050a684ecdf52674844cd7b1f48a0692e8078c99122d7` |
| FINAL-POST-ALL-TESTS | `8340965eb77d6afb479fa060c6dcd37e3f9c39bf84bd2693d1f844c6f9982e4f` | `63bcd12e1d890cc7ff0f409f54952bb9e0cdb017f52613c2a4bf2ac54e1e3caf` |

## Preserved source/build findings and limitation

SOURCE-PROVEN: caller PC is metadata; same-geometry invalidation preserves
observed revision while invalidating tickets/generations; changed geometry and
fatal/blocking invalidation discard revision provenance. Existing exact
reset/status/lease/generation/fence/geometry predicates, native packet
disposition and private terminal implementation remain intact.

BUILD-PROVEN internal pre-test validation: 51 private/mode cases, 102 runs,
66480 checks, 70 internal Python tests, bulk 80 × six-ring model, 30 compiled
original routes, seven ownership-read sites, zero required positive arguments,
and byte-identical repeat builds. The distinct public suite is documented in
BUILDING.md; unpublished capture tests are not represented as public CI tests.

The rev49 contract remains **formal C / informal C+, EXPERIMENTAL**. Successful
matching destructive D64 reset + observed DISABLED + 300-us drain remains an
experimental OLD-generation boundary, not a vendor specification. Time alone
never grants retirement. Physical success is not source-proven hardware release
authority or a guarantee for every future system/transition.

This internal classification reflects supporting technical evidence plus
successful physical validation; it does not represent vendor-documented
D11 rev49 release authority.

EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY

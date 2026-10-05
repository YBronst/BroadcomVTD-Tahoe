# Publication review — final local package

Status: local documentation and CI-helper reviews complete; final deterministic
release package verified. **Not published**. No Git repository, tag or GitHub
Release has been created for this project.

At the Phase-A pre-publication freeze, GitHub-hosted workflows had not yet run.
Hosted CI remains an external post-publication validation step; after publication,
GitHub Actions history is the authoritative hosted-run record.
KGP and ChatGPT review precedes any publication or manual deployment decision.

## Artifact separation

- Release: exact frozen physical PASS BroadcomVTD.kext 0.2.25, copied unchanged (0.2.17 preserved as historical release).
- Public build output: isolated CI build, not the source of the release asset.
- Source: 27 runtime/header/plist files plus exact frozen TargetGate protected
  by SHA-256 inventory; no runtime semantic edits.
- Documentation: adapted public texts, no local developer paths, live kernel
  pointer dumps, private captures, rejected versions or internal transcripts.
- Artwork: one canonical approved PNG, unchanged; README uses it directly.
  Forum drafts remain outside this repository, with no invented attachment URLs.

## Phase-A baseline local checks

These historical values precede the CI-helper hardening review. The final local
checks below supersede the relevant rows. Phase-A values are retained only as
the historical baseline, not as the current test totals.

| Check | Result |
| --- | --- |
| Exact released bundle / extracted ZIP executable | PASS: 147408 bytes, tested SHA-256 and UUID |
| Strict extracted kext signature / plist versions | PASS |
| Runtime source vs frozen .17 | Byte-identical |
| Public pinned-dependency builds | PASS, two forced builds byte-identical |
| Local rebuilt executable vs frozen identity | Identical on Xcode 26.5; still not substituted for the release |
| Actual-source private/mode cases | 51 cases, 102 optimized/sanitized runs, 66480 checks |
| Public Python tests | 39 PASS; unavailable internal trace methods explicitly omitted, not skipped |
| Public command count | 157 PASS |
| Compiled routes / ownership reads | 30 original routes / seven exact sites PASS |
| Metadata / zero positive arguments / frozen imports | PASS |
| Matching v0.2.17 tag check | PASS, using environment simulation only |
| Mismatched tag check | Rejected as expected; no tag created |
| Workflow YAML parse | PASS; hosted execution pending publication |
| Relative Markdown links / accidental local-data scan | PASS |

## Final local checks

| Check | Current result |
| --- | --- |
| Helper/build/test status | **PASS** |
| Actual-source private/mode coverage | **51 cases, 102 optimized/sanitized private runs, 66480 private checks** |
| Public Python suite / command count | **70 tests PASS / 157 commands PASS** |
| Focused helper regressions | 31 PASS, including required-tag, package-content, cache and sanitizer-environment cases |
| Fail-closed sanitizers | ASan/UBSan; intentional UBSan overflow exits nonzero (SIGABRT, -6) |
| CI-built signature / compiled checks | Strict verification, ad-hoc/no-team; 30 routes, seven ownership-read sites PASS |
| Repeat CI builds | Two forced same-environment bundles byte-identical; not physically validated CI artifacts |
| Frozen runtime source / TargetGate | All 28 protected inventory entries unchanged |
| Final package consistency | **PASS**: all packaged repository files byte-identical; no duplicate/missing/extra members |
| Tested KEXT outside / extracted from ZIP | **PASS**: exact complete frozen bundle inventory, 147440-byte executable, SHA-256 and UUID |
| Final SHA256SUMS / hardened verify_release.py | **PASS**; normal and exact required-tag environment validation |

The current 70 public Python tests are distinct from the historical 70 internal
pre-test tests. No unpublished capture coverage or hosted CI execution is implied.

## Final publication package

ZIP: `Release/BroadcomVTD-Tahoe-v0.2.17.zip`, **68258 bytes**,
SHA-256 `d01a69be6b7c93db7bd7f819c24a6ea8b30c5d5644487ee59754f918e82da6d7`.
The ZIP deliberately omits the hero and raw traces. It contains the exact tested
kext, installation/release notes, identity and project/dependency license notices.

Packaging uses sorted members, fixed 1980-01-01 timestamps, stable Unix file
modes, and DEFLATE level 9 without comments or extra metadata. Two independent
in-memory encodings were byte-identical; one final archive replaces the stale
Phase-A container. Release/SHA256SUMS records the final identity.
Only container/documentation bytes changed: the KEXT was not rebuilt, modified,
resigned or replaced, and Release/identity.json is unchanged. The ZIP container
is not itself physically tested; its exact embedded KEXT is.

Historical Phase-A package (superseded, **not the final asset**): **68026 bytes**,
SHA-256 `4cb24f301adbc9df5d4ead2adf94b34fd27c1be042e8dfd82e039b79d8f03608`.
That earlier snapshot predated the reviewed publication-only documentation edits.

## Review points retained explicitly

- Physically tested OS: **macOS Tahoe 26.6.2 (25G83)**. Project scope:
  **macOS Tahoe / Darwin 25.x**; the campaign does not cover every Tahoe release.
- OCLP-CustoMac **3.0.3** with Modern Wireless root patches is the tested patch
  environment, with **AppleVTD enabled / DisableIoMapper=false**;
  BroadcomVTD independently addresses DMA/IOMMU, not root-patch restoration.
- The later definitive final state has NoCredit **1071** and quarantine events
  **206**, but TX occupancy/pending/halt **zero**, RX live **240**. Earlier
  multi-Sleep NoCredit=0 and retirement count 46 remain time-specific facts.
- Ethernet disabled for Wi-Fi/Hotspot tests; adverse observations preserved with
  causality UNKNOWN and the one-off AirDrop failure not reproduced.
- Mieze/IntelLucy credited for architectural prior art, not copied implementation
  or co-development. Lilu/MacKernelSDK notices retained; project license does not
  relicense target driver fingerprints or external dependencies.
- Frozen legacy embedded KMOD 0.2.5 field is disclosed in BUILDING.md; released
  plist/Lilu/public version is 0.2.25. No cosmetic runtime edit was made.
- Runtime native-passthrough behavior has source/host coverage, not a newly
  claimed dedicated .17 hardware campaign.
- Rev49 remains **formal C / informal C+, EXPERIMENTAL**:
  **EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY**.
- No CI artifact may replace the official tested asset. No workflow publishes.

Remaining external validation: actual GitHub-hosted CI execution after authorized
publication; this phase validates its scripts locally without pretending the
remote service has already run. No missing build dependency was hidden by a skip.

## Publication boundary

Documentation and CI-helper reviews are complete. Reviewed third-party notices
are included unchanged. The local GitHub Release draft now records the final ZIP
size/SHA-256; no release has been created. Deterministic packaging, checksum and
package-content verification have passed. The repository and package are locally
ready for publication, subject to KGP authorization.

No Git initialization, commit, tag, remote, push, GitHub Release or deployment
was performed. GitHub publication remains a separate authorized step.

# Public build and CI

## Two different artifacts

`Release/BroadcomVTD.kext` is the exact frozen, physically tested bundle.
`Release/BroadcomVTD-Tahoe-v0.2.17.zip` is the user-facing release package
containing that exact tested bundle. Physical validation applies to the KEXT,
not the ZIP container. CI/build output must never replace the tested KEXT inside
the release package.
`build/poc-0.2.25/BroadcomVTD.kext` is a **CI BUILD — NOT PHYSICALLY VALIDATED**.
No make target writes into Release/.

The `POC/` source organization is retained. Runtime files are byte-identical to
the frozen implementation; only build/test/documentation integration is adapted.
`Config/frozen-source-sha256.json` protects that identity. The generated target
gate is checked in byte-for-byte under Config/, so building does not require
obtaining or redistributing a proprietary Apple driver.

## Build locally

Requirements: macOS, full Xcode with its command-line tools selected, Python 3.9
or later, and make. Internet access is required for initial dependency retrieval
unless the required hash-matching dependency archives are already available
through `--archive-cache`.

```sh
python3 -B tools/fetch_dependencies.py
make -j2
make test
make verify
make repeat
```

Dependencies are downloaded without Git into ignored `.deps/`. Exact Acidanthera
commit IDs **and archive SHA-256** values are in `Config/dependencies.lock.json`:

- Lilu 1.7.2, `e4748cc081bf060302c7d3c44a643ce1d11b7e1d`.
- MacKernelSDK, `05094e5e88cec7caedbfb35e8449ed0db94bf95b`.

Missing inputs and hash mismatches fail; they do not turn into skipped builds.
The fetcher refuses to trust an existing modified dependency directory. For an
offline build, supply previously downloaded, hash-matching `Lilu.tar.gz` and
`MacKernelSDK.tar.gz` with `--archive-cache DIRECTORY` on a fresh tree. No
third-party checkout is altered. Keep dependency license notices intact.

The compiler/kernel flags, x86_64 target, linked platform metadata and Lilu
bootstrap are preserved. Kernel headers come from MacKernelSDK, not a private
developer path. Xcode tools and its host-test SDK are located with `xcrun`.
Compiler warnings covered by the build remain errors (`-Wall -Wextra -Werror`,
with the existing explicit compatibility suppressions).

## What public CI verifies

`Build and regression tests` (`.github/workflows/ci.yml`) runs on push,
pull_request and workflow_dispatch. It uses an Intel macOS runner, pinned action
revisions and hash-pinned source dependencies, with read-only repository
permissions and no persisted checkout credentials.

It verifies the official release/source identity, executes actual-source
private-TX/mode host tests with ASan/UBSan, runs the public regression suite,
performs two forced builds, requires all kext bundle bytes to match between
those builds, and checks compiled routes, seven ownership-read roles,
x86_64/kext type, bundle/version/Lilu metadata and strict ad-hoc signature.
It also compares imports to the frozen tested executable's import inventory.

`Release tag identity` additionally runs on `v*` tag pushes and manual dispatch.
It requires the tag to match the source/bundle/release version and exact official
archive identity. It does not auto-bump versions, create releases or upload a
replacement official asset. A future version requires deliberate reviewed
identity updates; no tag was created during Phase A.

Artifacts are explicitly **CI-BUILD-NOT-PHYSICALLY-VALIDATED**. No workflow has
write permission to publish a GitHub Release. The README badge will represent
actual hosted results only after repository publication and workflow execution.

## Local Phase-A result

PASS with Xcode 26.5 (17F42), macOS host SDK 26.5 and the pinned archives:

- 51 private/mode cases × optimized/ASan+UBSan = 102 runs, **66480 checks**.
- **39 public Python tests**, plus TX/RX/read-policy/completion/pressure C++
  regressions and admission/RX cleanup sanitizer cases; 157 commands PASS.
- Bulk model: 80 × six rings, with no credit accumulation; native Free/requeue,
  failed terminal and wrong-generation cases retained.
- 30 compiled original routes, seven exact ownership-read sites, x86_64,
  bundle ID/version/signature and frozen-import checks PASS.
- Two forced public builds: bundle byte-identical. On this local toolchain,
  the executable also matches the frozen hash and UUID. The official release
  was copied independently from the frozen bundle, never taken from that build.

At the Phase-A pre-publication freeze, GitHub-hosted Actions had not yet run.
After publication, the README badge and GitHub Actions history are the
authoritative hosted-run status.

The workflow currently targets `macos-15-intel`. GitHub-hosted runner images
are maintained environments, not frozen VMs; toolchain/image updates can expose
compatibility differences or change build bytes. Cross-Xcode/cross-runner byte
identity is not promised. See the
[GitHub runner documentation](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
for runner reference.

## Honest limits and preserved details

- Six private historical-capture methods are intentionally not shipped; see
  `Config/public-test-scope.json`. The public suite is not claimed to reproduce
  all 70 internal pre-test Python tests or replay unpublished raw captures.
  No unavailable test is quietly marked skipped. The golden 578-chain
  actual-source host model remains in the public suite.
- Import checks use the frozen inventory. They do not pretend CI has the exact
  live Tahoe kernel/OCLP provider set or prove live linking/DMA safety.
- The frozen `POC/Module.cpp` retains its historical `KMOD_EXPLICIT_DECL`
  string `0.2.5`. The physical distribution's plist versions, Lilu module
  version and public release are **0.2.25**. This embedded legacy field is
  documented rather than cosmetically changed in the tested runtime source.
- The rev49 reset/DISABLED/300-us contract remains experimental. Host tests,
  successful builds and a green badge are not hardware release authority.

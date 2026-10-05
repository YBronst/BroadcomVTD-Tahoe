# Source, release and artwork provenance

## Independent implementation and frozen release

The public runtime source represents BroadcomVTD 0.2.25. All 27 POC
runtime/header/plist files and the relocated frozen generated TargetGate header
are protected by `Config/frozen-source-sha256.json`. Public adaptations affect
build/dependency locations, host test selection, checks and documentation—not
runtime behavior. No rejected experiment or historical source archive is imported.

The released bundle comes directly from the frozen physical PASS artifact for 0.2.25:

- executable 147440 bytes;
- SHA-256 `1fe9a8d4d06b8ed8e0638ccaa911f677c152d797b9273fc1c7d8039ad81e96b8`;
- UUID `C70785F0-D8E1-337F-A295-B82DA18F94ED`;
- plist versions 0.2.25; bundle `local.kgp.BroadcomVTD`.

`Release/identity.json` pins every bundle file. The public ZIP is the active
user-facing distribution for v0.2.25, while v0.2.17 is preserved as a previous
historical frozen release. CI never overwrites the official asset. No cosmetic
1.0.0 rebuild, Git initialization or remote publication was performed during Phase A.

## Target gate provenance

`Config/TargetGate.hpp` is byte-identical to the generated header compiled into
the tested .17 artifact. It contains 30 exact route signatures, private-layout
comparison windows and seven ownership-read roles for the pinned native UUID.
These small compatibility fingerprints are data checked against the loaded
driver; no proprietary driver executable or native algorithm implementation is
distributed. The actual AirPortBrcmNIC driver remains an external component
supplied by the user's Modern Wireless root-patch environment and is not
distributed by BroadcomVTD-Tahoe. No gate was relaxed for public CI.

## Documentation and evidence adaptation

The public physical-validation document is adapted from the frozen internal
`BroadcomVTD-0.2.17-PHYSICAL-VALIDATION.md`, SHA-256
`60d56d266edcc108674c5c179dae83afb2d29324b24b170ce79152da40de2002`.
It preserves the findings, caveats and four capture hashes, but omits developer
absolute paths, live kernel pointer values and internal transcript/research data.
It is **not claimed to be byte-identical** to the internal report. Raw capture
files and complete internal evidence remain separate.

## Licensing review scope

The selected source/build inspection identifies independently implemented
BroadcomVTD code, Lilu bootstrap/header dependency and MacKernelSDK kernel
interface/build inputs. Mieze's IntelLucy Tahoe AppleVTD DMA work is credited as
architectural prior art; it is not BroadcomVTD co-development and no IntelLucy
implementation is copied or linked.

The chosen BSD-3-Clause project license is scoped to independent contributions.
Lilu's BSD notice and MacKernelSDK's APSL notice/source location are retained
separately in THIRD_PARTY_NOTICES.md and Licenses/. No SDK, Lilu.kext, OCLP
payload, Apple driver, firmware or third-party source checkout is vendored.
These notices do not grant rights to third-party operating-system software or
supersede its terms. Final licensing/publication review remains with KGP.

## Approved hero artwork

The canonical hero artwork was generated with OpenAI ChatGPT image generation
under KGP direction and approved by KGP for project use.
The approved asset is `Assets/BroadcomVTD-Tahoe-Hero.png`:

- PNG, **1672 × 941** pixels;
- **1971571 bytes**;
- SHA-256 `af092739e5f7df283489d5c4294ed292a29a39f5d2a025410fb7731ce02709de`.

It was supplied already complete. The Phase-A repository preparation did not
resize, recompress, rename or edit it. The README references that one public asset.
Forum drafts designate the same image for KGP's later manual attachment; no
attachment/thread URLs were invented. The release ZIP does not duplicate it.

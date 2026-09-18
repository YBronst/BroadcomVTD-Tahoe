<p align="center">
  <img src="Assets/BroadcomVTD-Tahoe-Hero.png"
       alt="BroadcomVTD-Tahoe"
       width="100%">
</p>

[![Build and regression tests](https://github.com/kgp-macPro/BroadcomVTD-Tahoe/actions/workflows/ci.yml/badge.svg)](https://github.com/kgp-macPro/BroadcomVTD-Tahoe/actions/workflows/ci.yml)
[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue)](LICENSE)

BroadcomVTD-Tahoe helps supported legacy Broadcom Wi-Fi operate under macOS Tahoe
while keeping **AppleVTD/IOMMU enabled**. It is an experimental Lilu plugin for
the restored AirPortBrcmNIC driver. It selects its runtime path from the verified
mapper identity—not by waiting for Wi-Fi or DMA errors.

**v0.2.17 is a physically validated experimental release.** The project is
BroadcomVTD-Tahoe; the actual extension remains **BroadcomVTD.kext**, bundle ID
`local.kgp.BroadcomVTD`. This exact tested binary retains version **0.2.17**;
there was no cosmetic 1.0.0 rename or rebuild.

[Download the official v0.2.17 release](https://github.com/kgp-macPro/BroadcomVTD-Tahoe/releases/tag/v0.2.17).
Its official ZIP contains the exact physically tested **BroadcomVTD.kext**.
GitHub Actions CI artifacts are not substitutes.

## What BroadcomVTD does — and does not do

**This is an independent project, not part of OpenCore Legacy Patcher or
[OCLP-CustoMac](https://github.com/kgp-macPro/OCLP-CustoMac).**
OCLP-CustoMac provides the Modern Wireless root-patch environment that restores
the legacy Broadcom stack under Tahoe. BroadcomVTD addresses the additional
kernel-resident Tahoe runtime DMA/IOMMU compatibility problem observed when
that restored AirPortBrcmNIC stack operates with AppleVTD enabled.

For users choosing an AppleVTD-enabled setup, BroadcomVTD provides corrective
DMA handling for the supported restored driver while preserving native Broadcom
packet free/requeue ownership. It checks the actual mapper before enabling that
correction; it is not a Wi-Fi fault detector.

BroadcomVTD does **not** install or replace Modern Wireless root patches, provide
a replacement Wi-Fi driver, enable VT-d or AppleVTD, edit OpenCore configuration
or DMAR, or change global IOMMU policy. It does not patch Aquantia Ethernet or
AirportItlwm.

**This project does not make AppleVTD a general macOS Tahoe requirement.**
Nor does it establish that every Broadcom card needs BroadcomVTD, or that enabling
VT-d in firmware necessarily breaks Broadcom Wi-Fi. Its physical validation and
compatibility gates define the supported scope below.

## OpenCore `DisableIoMapper` and BroadcomVTD runtime selection

Start with the OpenCore setting **`DisableIoMapper`**. This is an OpenCore
configuration control, **not a BroadcomVTD runtime-state variable**.

### DisableIoMapper=false

This is the normal macOS-oriented configuration **without OpenCore's
mapper-disabling workaround**. `DisableIoMapper=false` means that OpenCore does
not apply its XNU IOMapper-disabling quirk. This permits the normal macOS
system-IOMapper / AppleVTD-capable path, but does not by itself prove that
AppleVTD is active. **`DisableIoMapper=false` permits but does not prove the mapper.**

If BroadcomVTD verifies that actual system AppleVTD source for the supported
target at runtime, it selects its **AppleVTD-aware corrective path**. If the
source does not qualify, the setting alone cannot activate correction. The
physically validated v0.2.17 configuration used `DisableIoMapper=false` **and**
successfully verified system AppleVTD.

This is BroadcomVTD-Tahoe's central purpose: the supported restored legacy
Broadcom stack can work with the system mapper, without requiring the
system-wide `DisableIoMapper=true` workaround solely for Broadcom Wi-Fi.

This can be useful when legacy Broadcom Wi-Fi shares a system with **Aquantia
10GbE or other devices using AppleVTD**. The supported restored Broadcom path
can coexist with the mapper, so it need not be disabled globally solely to
accommodate Wi-Fi. AppleVTD remains available for other devices when macOS
actually establishes and uses it. BroadcomVTD does **not** patch
`AppleEthernetAquantiaAqtion` or Aquantia hardware; this is not a claim that every
Aquantia configuration requires AppleVTD or was tested by this project.

### DisableIoMapper=true

This is the **OpenCore compatibility workaround historically used for the
restored Broadcom Tahoe environment**: the quirk disables XNU IOMapper support
for VT-d. When that control takes effect and BroadcomVTD has no verified system
AppleVTD source to select, its AppleVTD-specific corrective machinery stays
inactive. AirPortBrcmNIC continues on the **native AirPortBrcmNIC path —
BroadcomVTD correction inactive**.

That describes BroadcomVTD's involvement in the driver path. It is not proof
that the whole machine has no IOMMU translation, or that device DMA uses
identity mappings. The historical mapper-disabled baseline remains a separate
configuration; BroadcomVTD does not change it automatically.

### How BroadcomVTD selects its runtime path

BroadcomVTD **does not parse or edit `config.plist`** to decide its mode. It
verifies the **actual runtime mapper/source identity**, together with the
required target/ABI/provider checks. Merely finding an AppleVTD-named service
is not sufficient. The internal names below describe BroadcomVTD, not OpenCore
settings or named macOS operating modes.

| Actual mapper selection for the supported target | BroadcomVTD behavior | Internal v0.2.17 diagnostic name |
| --- | --- | --- |
| Verified system AppleVTD source selected | **AppleVTD-aware corrective path.** Corrective RX/TX DMA, private-TX backing and lifetime handling are enabled. This is the physically validated path. | `APPLEVTD_CORRECTIVE_EXPERIMENTAL` |
| No verified system AppleVTD source selected | **Native AirPortBrcmNIC path — BroadcomVTD correction inactive.** Native packet/DMA behavior continues; corrective RX mapping and private-TX lifetime handling are inactive. | `NATIVE_PASSTHROUGH` |

**Automatic mode selection is not automatic failover.** BroadcomVTD does not
wait for Wi-Fi, DMA, packet, timeout or crash symptoms and then switch modes.
It does not wait for an AppleVTD-related failure before selecting its corrective
path. The decision is based on verified mapper identity, not Wi-Fi success or
failure; corrective-path failures do not trigger a switch back to native mode.

Selection occurs during target binding. A later binding opportunity can qualify
a mapper that was not previously selected; this is identity-based binding, not
network-health monitoring. Once corrective mode is selected, v0.2.17 does not
downgrade it to native mode on an allocation failure, stop or failed rebind.

`NATIVE_PASSTHROUGH` means only that BroadcomVTD's verified system AppleVTD source
was not selected and correction is inactive. It does **not** mean “AppleVTD is
active and no problem was found,” IOMMU bypass, identity mapping or a generic
direct-DMA mode. It does not prove that every system/device mapper is absent.
The native path has source/host evidence; **it did not receive a new dedicated
v0.2.17 physical campaign**.

### Hardware capability is not runtime mapper identity

**VT-d/IOMMU capability** is the platform's ability to translate device DMA
addresses. **AppleVTD / system IOMapper** is the macOS runtime mapping environment
relevant to this Intel setup. Firmware capability or enablement alone does not
prove that macOS has instantiated the mapper BroadcomVTD requires.
`DisableIoMapper=true` does not remove that firmware capability, and `false`
does not create a mapper. These are distinct layers—not Apple “normal” and
“failover” modes.

## Physically validated configuration

| Component | Tested configuration |
| --- | --- |
| Motherboard | ASUS WS X299 Sage/10G |
| Wi-Fi | BCM943602CDP, PCI `14e4:43ba`, D11 rev49 |
| macOS | **Tahoe 26.6.2 (25G83)**; project scope: Tahoe / Darwin 25.x |
| Wireless restoration | **OCLP-CustoMac 3.0.3 with Modern Wireless root patches** |
| Lilu | 1.7.2 used for physical validation |
| IOMMU | AppleVTD enabled; `DisableIoMapper=false` |
| BroadcomVTD | Exact frozen v0.2.17 release binary; no positive BroadcomVTD arguments |

BCM943602CDP is the **physically validated reference**, not a device-only
authorization rule. Other compatible legacy Broadcom hardware is not
automatically excluded, but must satisfy the target, ABI, private-layout,
provider and runtime mapper gates. This is not a universal BCM94360-family
support or physical-validation claim.

A matching card model alone is insufficient. The physically pinned
AirPortBrcmNIC UUID is `E4678FEB-1DC1-35CC-9306-48021083D04A`; its exact supported
ABI must also match. See [scope and provenance](Docs/PROVENANCE.md).

## Requirements and scope

- Intel/x86_64 Hackintosh; project scope is **macOS Tahoe / Darwin 25.x**.
  The physically tested OS is **Tahoe 26.6.2 (25G83)**, not every Tahoe point release.
- A compatible Modern Wireless restoration environment. Physical validation used
  **OCLP-CustoMac 3.0.3 with Modern Wireless root patches**.
- The exact supported AirPortBrcmNIC target/ABI/private-layout/provider gates;
  a Broadcom vendor or card-family name alone does not establish compatibility.
- **Lilu 1.7.2** was used for physical validation. A compatible later Lilu version
  may also be used. BroadcomVTD must load **after Lilu**.
- For the physically validated corrective setup, **AppleVTD enabled and
  `DisableIoMapper=false`**, with runtime mapper verification as explained above.
  This requirement belongs to the corrective setup, not to macOS Tahoe generally.

## Installation with OpenCore

1. Keep a bootable backup of your existing EFI and a recovery route. Read the
   experimental limitation below before changing a working system.
2. Establish the supported Tahoe Modern Wireless environment separately. This
   project neither supplies OCLP payloads nor changes its patch/security setup.
3. Use **BroadcomVTD-Tahoe-v0.2.17.zip from the project's Release assets**, not a
   GitHub Actions build. Verify the executable identity below after extraction.
4. Manually copy `BroadcomVTD.kext` to `EFI/OC/Kexts`. Ensure Lilu is present and
   enabled as described above. Add BroadcomVTD under OpenCore `Kernel -> Add`
   **after Lilu**: BundlePath `BroadcomVTD.kext`, ExecutablePath
   `Contents/MacOS/BroadcomVTD`, PlistPath `Contents/Info.plist`, Enabled `true`,
   Arch `x86_64`. Limit it to Darwin 25.x/Tahoe in a multi-OS EFI if needed.
5. Retain the order and prerequisites of your existing OCLP Modern Wireless
   stack. BroadcomVTD does not replace IOSkywalkFamily, IO80211FamilyLegacy,
   AMFIPass or their existing setup. It is a **Lilu plugin**, not an AirportItlwm
   extension. Do not install duplicate BroadcomVTD copies.
6. Validate the OpenCore configuration and reboot manually when ready. Do not
   live-load/unload this resident kext. Start with Wi-Fi, then one Sleep/Wake,
   then your usual wireless functions. To roll back, restore your known-good
   EFI/entry; this project does not automate system configuration changes.

BroadcomVTD requires no boot arguments for normal operation. The optional
`-brcmvtdoff` boot argument disables the plugin. **This does not remove boot
arguments required by other components**, including your root-patch setup.
The validated configuration uses `DisableIoMapper=false`; BroadcomVTD does
not parse or modify OpenCore's configuration and selects its runtime mode
from the actual mapper identity.

Corrective mode includes the experimental reset/drain policy automatically.
See [architecture](Docs/ARCHITECTURE.md) for the ownership and lifetime details.
It is not an option activated in response to a failure.

## What was tested

Wi-Fi, repeated Sleep/Wake, bidirectional AirDrop, AirPlay, Screen Mirroring,
tested AWDL/Continuity functions including Continuity Camera, and Personal
Hotspot passed the reported controlled campaign. Bidirectional functions were
tested bidirectionally where applicable.

**AirPlay / Screen Mirroring:** On some Tahoe systems, these features may also depend on SMBIOS, graphics configuration and framework-level feature gating rather than the Broadcom Wi-Fi / AppleVTD path alone. If Wi-Fi/AWDL is otherwise working, see [FeatureUnlock-Tahoe](https://github.com/kgp-macPro/FeatureUnlock-Tahoe). It is a separate project, not a requirement for Broadcom Wi-Fi; feature unavailability alone does not establish a Broadcom Wi-Fi/AWDL or AppleVTD failure.

**Ethernet was disabled during Wi-Fi and Personal Hotspot validation.** Those
passes were not silently carried by the X550 Ethernet interfaces.

The definitive post-all-tests capture ended in corrective mode with:

| Health result | Final state |
| --- | --- |
| TX live / quarantined mappings | **0 / 0**; mapping snapshot empty |
| TX halt / pending OLD private records | **0 / 0** |
| Historical quarantine events | 206—not current occupancy |
| Cumulative NoCredit pressure events | 1071—not a permanent failure |
| Later reservations / submissions after pressure | 23748 / 23748 |
| RX | enabled, not halted; **240 live** |
| First permanent failure | not captured |

Temporary mapping pressure remained non-latching: later traffic continued and
TX occupancy returned completely to zero. This does **not** claim that every
individual NoCredit event was separately traced to completion. RX accounted for
38620 prepared/mapped, 37660 normally completed and 720 reset-completed records:
`38620 - 37660 - 720 = 240`.

### Sleep/Wake: the important change

Earlier experiments could conservatively retain an OLD private TX mapping
across certain native bulk resets. v0.2.17 generalized the reset lifetime model,
not a Sleep-, FIFO- or application-specific exception. Native code still owns
packet free/requeue behavior; BroadcomVTD ends only the eligible OLD DMA lifetime.

Physical tracing confirmed the six-ring native core-reset path and successful
retirement of an active OLD private DMA lifetime. Repeated Sleep/Wake completed
with zero retained TX mappings, no mapper halt, no pending OLD records and
RX live at 240.

The controlled post-wake validation passed Wi-Fi, bidirectional AirDrop, AirPlay,
Screen Mirroring, Continuity Camera and Personal Hotspot. See the
[detailed physical-validation report](Docs/PHYSICAL_VALIDATION_v0.2.17.md) for the
exact serial/generation/ticket and Stage17 -> Stage13 evidence, complete campaign
observations and evidence boundaries.

## Experimental hardware boundary

Internally, the available rev49 hardware evidence is classified as
**formal C / informal C+**. In practical terms, the implementation has supporting
technical evidence and successful physical validation, but the reset/drain retirement
contract is not a vendor-documented D11 rev49 hardware specification.

The assumed boundary remains **EXPERIMENTAL**: a positively successful matching
destructive D64 reset, observed DISABLED acknowledgement and the existing
**300-us drain**, with the ownership/generation checks intact.
Time alone never authorizes retirement.

```text
EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY
```

Successful testing is not a Broadcom/Apple hardware specification or a generic
production-safety claim. Use only with informed acceptance of this limitation.

## Exact release identity

```text
BroadcomVTD.kext — local.kgp.BroadcomVTD — 0.2.17
x86_64 executable: 147408 bytes
SHA-256: 2a8f9641a9c9856b1e45899f31332a68ad2cf519e94951ca128bb3f262c3c349
UUID: D0B214B8-F096-3BC6-99AD-F1E9D8E788B6
```

[Release notes](RELEASE_NOTES_v0.2.17.md) · [Build and CI](Docs/BUILDING.md) ·
[Architecture](Docs/ARCHITECTURE.md) · [Provenance](Docs/PROVENANCE.md) ·
[License](LICENSE)

CI performs source/build/regression/identity checks. Uploaded artifacts are
**CI BUILD — NOT PHYSICALLY VALIDATED**. They never replace the frozen official
release, even if a local rebuild happens to produce identical bytes.

## Credits

- **[KGP / kgp-macPro](https://github.com/kgp-macPro):** project concept/direction, system integration, hardware experimentation,
  physical validation, release maintenance and documentation.
- **OpenAI ChatGPT:** research and architecture collaboration, evidence analysis,
  test strategy, independent source review and technical documentation.
- **OpenAI Codex CLI:** source implementation, local binary/source analysis,
  build/validation tooling and evidence/report generation, under KGP direction
  and independent ChatGPT review.
- **[Mieze](https://github.com/Mieze) / [IntelLucy](https://github.com/Mieze/IntelLucy):** important architectural prior art, especially Tahoe
  AppleVTD DMA and mapper-aware packet-lifetime work. BroadcomVTD is an independent
  AirPortBrcmNIC implementation with a different final private-TX-backing
  architecture—not an IntelLucy source derivative or a co-developed project.
- **[Acidanthera](https://github.com/acidanthera):** Lilu framework and MacKernelSDK build infrastructure.
- **[OpenCore Legacy Patcher developers](https://github.com/dortania/OpenCore-Legacy-Patcher):** the Modern Wireless environment used
  to restore legacy Broadcom Wi-Fi under Tahoe. BroadcomVTD is independent of OCLP.

BSD-3-Clause for independent project contributions; dependency notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Community project; no implied
endorsement by Apple, Broadcom, OpenAI, Acidanthera, Mieze or the OCLP developers.

# BroadcomVTD-Tahoe v0.2.27 — AirportBrcmFixup Coexistence & BCM94352HMB Support

BroadcomVTD-Tahoe v0.2.27 introduces seamless Lilu multiroute coexistence with AirportBrcmFixup and expands hardware support for Broadcom BCM94352HMB / BCM4352 (`14e4:43b1`).

## Key Changes in v0.2.27

- **AirportBrcmFixup Coexistence via Lilu Multiroute:** Migrated hook installation to Lilu's `routeMultipleLong` API, enabling concurrent and conflict-free routing of `AirPort_BrcmNIC::start()` alongside `AirportBrcmFixup`.
- **Expanded Hardware Support:** Added verified support for BCM94352HMB / BCM4352 (PCI ID `14e4:43b1`, rev 3, D11 core rev 42).
- **Clean Separation of Concerns:** BroadcomVTD strictly focuses on DMA/TX/RX VTD stability and AppleVTD IOMMU kernel protection, delegating Wi-Fi injection, ASPM, country code, and ARPT provider renaming to `AirportBrcmFixup`.

## Validated Configurations

- Hardware:
  - BCM943602CDP / BCM43602 (`14e4:43ba`), D11 rev 49
  - Apple BCM94360CS2 / ASUSTeK BCM4360 (`14e4:43a0`, `14e4:43a3`), D11 rev 42/43
  - Broadcom BCM94352HMB / BCM4352 (`14e4:43b1`), D11 rev 42
- Environment: macOS Tahoe 26.6.2 with **OCLP-CustoMac 3.0.3 with Modern Wireless root patches**.
- Configuration: `DisableIoMapper=false` with active AppleVTD.

## Important Limitation

EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY

Date: 21 September 2026

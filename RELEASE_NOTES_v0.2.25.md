# BroadcomVTD-Tahoe v0.2.25 — BCM4360 Support & Traffic Freeze Fix

BroadcomVTD-Tahoe v0.2.25 expands hardware compatibility and addresses critical runtime DMA stability issues while operating under macOS Tahoe with AppleVTD/IOMMU enabled.

## Key Changes in v0.2.25

- **Expanded Hardware Support:** Added compatibility for BCM4360 wireless cards (D11 revisions 42 and 43, PCI IDs `14e4:43a0` and `14e4:43a3`).
- **Traffic Freeze Fix (Mapping Quarantine Resolution):** Resolved a critical issue causing traffic to freeze (~30 seconds after connection or under load) due to unhandled private-TX buffer mapping quarantine conditions and reset-lifetime state tracking.
- **Improved Reset & Recovery Handling:** Enhanced private-TX ring lifetime management and generation/ticket invalidation during native resets, ensuring sustained throughput without latching NoCredit halts.

## Validated Configuration

- Hardware: BCM94360 / BCM4360 (`14e4:43a0`, `14e4:43a3`) and BCM943602CDP (`14e4:43ba`), D11 rev 42/43/49.
- Environment: macOS Tahoe 26.6.2 with **OCLP-CustoMac 3.0.3 with Modern Wireless root patches**.
- Configuration: `DisableIoMapper=false` with active AppleVTD.

## Important Limitation

EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY

Date: 18 September 2026

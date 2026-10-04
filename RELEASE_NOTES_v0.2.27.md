# BroadcomVTD-Tahoe v0.2.27 — Expanded Broadcom Hardware Support

BroadcomVTD-Tahoe v0.2.27 expands hardware support for Broadcom BCM94352HMB / BCM4352 (`14e4:43b1`) and Apple BCM94360CS2 / ASUSTeK BCM4360 (`14e4:43a0`, `14e4:43a3`).

## Key Changes in v0.2.27

* **Expanded Hardware Support:** Added and physically tested support for BCM94352HMB / BCM4352 (PCI ID `14e4:43b1`, D11 core rev 42).

* **Additional BCM94360 Support:** Added and physically tested support for Apple BCM94360CS2 / ASUSTeK BCM4360 (PCI IDs `14e4:43a0`, `14e4:43a3`, D11 core rev 42/43).

* **TX Buffer Window Update:** Increased the internal buffer window from `26` to `27`. This change was physically validated and resulted in the maximum Wi-Fi throughput observed on the test system.

* **DMA/TX/RX VTD Stability:** BroadcomVTD focuses on DMA/TX/RX handling and AppleVTD IOMMU kernel protection.

## AirportBrcmFixup Compatibility

`AirportBrcmFixup` is not required for BroadcomVTD and is not part of the validated configuration.

On the tested configuration, enabling `AirportBrcmFixup` interferes with the correct operation of BroadcomVTD and may result in kernel panic. It is therefore recommended to keep `AirportBrcmFixup` disabled when using this release.

## Validated Configurations

* **BCM943602CDP / BCM43602** (`14e4:43ba`), D11 rev 49

* **Apple BCM94360CS2 / ASUSTeK BCM4360** (`14e4:43a0`, `14e4:43a3`), D11 rev 42/43

* **Broadcom BCM94352HMB / BCM4352** (`14e4:43b1`), D11 core rev 42

* **Environment:** macOS Tahoe 26.6.2 with **OCLP-CustoMac 3.0.3 with Modern Wireless root patches**.

* **Configuration:** `DisableIoMapper=false` with active AppleVTD.

## Important Limitation

EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY.

Support and validation are limited to the hardware and software configurations described above.

Date: 4 October 2026


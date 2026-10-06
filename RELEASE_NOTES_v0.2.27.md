# BroadcomVTD-Tahoe v0.2.27 — Expanded Broadcom Hardware Support

BroadcomVTD-Tahoe v0.2.27 expands hardware support for Broadcom BCM4352 (`14e4:43b1`) and Apple / Fenvi BCM94360CS2 / BCM94360CD / ASUSTeK BCM4360 (`14e4:43a0`).

## Key Changes in v0.2.27

* **Expanded Hardware Support:** Added and physically tested support for BCM94352HMB / BCM4352 (PCI ID `14e4:43b1`, D11 core rev 43).

* **Additional BCM94360 Support:** Added and physically tested support for Apple/Fenvi BCM94360 / ASUSTeK BCM4360 (PCI IDs `14e4:43a0`, D11 core rev 42).

* **Additional BCM943602 Support:** Added and physically tested support for Apple BCM943602CDP / BCM943602CS (`14e4:43ba`), D11 rev 49).

* **TX Buffer Window Update:** Increased the internal buffer window from `26` to `27`. This change was physically validated and resulted in the maximum Wi-Fi throughput observed on the test system.

* **DMA/TX/RX VTD Stability:** BroadcomVTD focuses on DMA/TX/RX handling and AppleVTD IOMMU kernel protection.

## AirportBrcmFixup Compatibility

`AirportBrcmFixup` is not required for BroadcomVTD and is not part of the validated configuration.

On the tested configuration, enabling `AirportBrcmFixup` interferes with the correct operation of BroadcomVTD and may result in kernel panic. It is therefore recommended to keep `AirportBrcmFixup` disabled when using this release.

## Validated Configurations

* **BCM943602CDP / BCM43602CS** (`14e4:43ba`), D11 rev 49

* **Apple / Fenvi BCM94360 / ASUSTeK BCM4360** (`14e4:43a0`), D11 rev 42

* **Broadcom BCM94352** (`14e4:43b1`), D11 core rev 43

* **Environment:** macOS Tahoe 26.7.1 (25G241) with **OCLP-CustoMac 3.0.3 with Modern Wireless root patches**.

* **Configuration:** `DisableIoMapper=false` with active AppleVTD.

## Important Limitation

EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY.

Support and validation are limited to the hardware and software configurations described above.

Date: 4 October 2026


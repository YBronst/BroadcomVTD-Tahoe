# BroadcomVTD-Tahoe v0.2.27

Physically validated EXPERIMENTAL Tahoe release. Actual file:
`BroadcomVTD.kext`, identifier `local.kgp.BroadcomVTD`. See RELEASE_NOTES_v0.2.27.md
and https://github.com/kgp-macPro/BroadcomVTD-Tahoe for configuration and scope.

Back up your EFI. Establish the supported OCLP-CustoMac 3.0.3 Modern Wireless
root-patch environment separately. Manually add BroadcomVTD.kext to OpenCore's
Kexts and Kernel/Add configuration **after Lilu 1.7.2 or later**. Preserve your
existing wireless stack and other components' boot arguments. BroadcomVTD needs
zero positive arguments. No live load/unload; KGP/user controls reboot/testing.

Executable identity: 147424 bytes; SHA-256
`1fe9a8d4d06b8ed8e0638ccaa911f677c152d797b9273fc1c7d8039ad81e96b8`;
UUID `C70785F0-D8E1-337F-A295-B82DA18F94ED`.

Runtime selects actual verified AppleVTD identity, not OpenCore configuration
text. The tested setup keeps AppleVTD enabled and DisableIoMapper=false.

EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY

The successful reset/DISABLED/300-us premise is not a Broadcom/Apple rev49
specification. Use with informed acceptance and a known-good recovery route.
Do not substitute a CI artifact for this frozen physical release.

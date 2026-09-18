# Frozen runtime source

The historical `POC/` layout is preserved for source traceability. These runtime
C++/header files and Info.plist are byte-identical to physically validated 0.2.25.
See the [project README](../README.md), [build guide](../Docs/BUILDING.md), and
[physical validation](../Docs/PHYSICAL_VALIDATION_v0.2.17.md).

`Config/TargetGate.hpp` is the exact frozen generated compatibility fingerprint
header, relocated from the internal build output without changing its bytes.
No Apple driver binary is included. The terminal remains:

EXPERIMENTAL; NOT REV49 SOURCE-PROVEN; NOT RELEASE AUTHORITY

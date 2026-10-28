# QcomXnuUsbDebug, vendored for hummer

Web: https://github.com/xiaoleGun/QcomXnuUsbDebug (MIT)
Vendored from: https://github.com/Lux12138/nabu-usb-cdc @ `627c83a`, path `module/`
Upstream baseline: `83be578`; the nabu fork's changes are `docs/module-changes.patch` there.
Licence: `LICENSE` (module) and `usb/LICENSE.m1n1` (DWC3 register descriptions).

This directory is the module as published, with one addition: `soc/Sm8750.h`, the
SM8750 (OnePlus Ace 5 Pro, hummer) profile. Nothing else is modified, so a future
re-sync can diff against `module/` of the source repository.

Build: `scripts/build-hummer-usbcdc.py` (replays the engine's osfmk startup
compile record, archives `build/hummer-usbcdc/libHummerUsbCdc.a`).

Status for hummer: compiles only for now. The XNU console hooks in `kernel/`
need the two darwin22 patches rebased from xnu-8796.141.3 to xnu-10063.141.1
(the engine's pin), and the archive has to be force-loaded. See
`docs/usb-cdc-port.md` in the device repository for the plan and the hardware
facts behind `soc/Sm8750.h`.

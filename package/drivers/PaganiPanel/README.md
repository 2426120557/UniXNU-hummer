# PaganiPanel

DSI brightness and display blanking for the OnePlus 13T AA594_P_7_A0020 panel. Enable with `pagani-panel=1`. Stage7Probe loads AppleARMPlatform and registers an owned subclass with genuine `AppleARMBacklight` ancestry; its constructor, storage size, destructor and 169-slot class extent are checked against the pinned 21H461 image. The subclass uses the ordinary IOService lifecycle and guards the unqualified extra virtual method.

`setProperties` accepts an integer `brightness` from 0 to the board's normal maximum, 0xdbb. The worker sends DCS 0x51 with the panel's low-byte-first brightness payload. Values are published only after command completion. The display PM callback requests DCS 0x28/0x29 for blanking and unblanking. These commands retain the existing clocks, PHY, DSC configuration and GOP scanout; they do not power down panel rails.

`Sm8750Platform/PeripheralIO.h` uses the DSI command FIFO, waits for the command path to be idle and preserves unrelated interrupt status. A completion timeout disables subsequent requests and leaves the outstanding command untouched. The first brightness command must succeed before the service advertises `backlight-control`.

The current refresh source remains the existing 60 Hz software presentation timer. Logical display-off pauses notifications, and display-on starts a fresh deadline. No panel timing, DSC mode or refresh rate is changed. TE/vblank synchronization and high-refresh modes require hardware measurements.

The portable tests cover packet byte order, register preservation, busy engines and completion timeouts. They do not verify physical luminance, CoreBrightness sandbox access, display-off/on behavior or coexistence with the firmware scanout. FadePeriod interpolation and full panel suspend/resume are not implemented.

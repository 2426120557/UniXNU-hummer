// Empty stubs for the offline component-load probe hooks.
//
// offline-component-load-probes.patch (bsd_init.c) references these two symbols
// unconditionally in DEVELOPMENT/DEBUG builds, inside native_mobile_graphics_load_probe().
// They are only *called* when the matching runtime boot arg is set
// ("offline-legacy-register" / "offline-mobile-graphics-client"), which a first
// boot never sets, so the stubs just satisfy the linker.
//
// v69: native_legacy_display_probe is no longer stubbed here.  The Stage 7
// display port (package/Stage7Probe.cpp, built into
// build/native-legacy-display/libNativeLegacyDisplay.late.a) provides the real
// implementation: it registers the IOMobileFramebufferLegacy backend class and
// allocates the display instance when offline-legacy-register=1 is set.  Two
// definitions of the same symbol cannot coexist in the force_load set, so the
// stub had to go the moment CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y landed.
//
// The sibling hook native_mobile_graphics_client_probe is NOT stubbed: XNU's own
// IOUserClient.cpo already defines it, so duplicating it here produced
// "duplicate symbol" at the final link.

// Intentionally empty: nothing left to stub.  The file stays because
// build-native-probe-stubs.py lists it and the archive must keep existing.

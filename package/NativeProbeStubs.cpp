// Empty stubs for the offline component-load probe hooks.
//
// offline-component-load-probes.patch (bsd_init.c) references these two symbols
// unconditionally in DEVELOPMENT/DEBUG builds, inside native_mobile_graphics_load_probe().
// They are only *called* when the matching runtime boot arg is set
// ("offline-legacy-register" / "offline-mobile-graphics-client"), which a first
// boot never sets, so the stubs just satisfy the linker.
//
// The real implementations live in a device's display provider; hummer does not
// have one yet (Adreno 830 / a8xx), so these are placeholders for bring-up.

// Only native_legacy_display_probe needs a stub here.
//
// The sibling hook native_mobile_graphics_client_probe is NOT stubbed: XNU's own
// IOUserClient.cpo already defines it, so duplicating it here produced
// "duplicate symbol" at the final link.

extern "C" void native_legacy_display_probe(void) {}

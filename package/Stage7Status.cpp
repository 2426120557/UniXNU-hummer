// SPDX-License-Identifier: BSD-2-Clause-Patent
// hummer: the Stage 7 display status surface, without the BootProgress driver.
//
// Stage7Probe.cpp (ported from IssacL04/UniXNU-Pagani_SM8750, same SM8750 and
// the same 21H461 kernelcache) reports its progress through three functions
// that live in their BootProgress driver:
//
//   boot_progress_display_status(detail, value)  record a stage line
//   boot_progress_display_handoff()              may the display take the framebuffer?
//   boot_progress_display_failed()               a first copy failed, resume diagnostics
//
// hummer deliberately does not build BootProgress: the engine gates
// XNU_BOOT_PROGRESS=1 on the audited UFS/owner-root, Virtio and
// source-corecrypto build (build-xnu-4k-baseline.sh), and the board already
// has its own diagnostic surface - the panel renderer of
// build-hummer-console-probe.py (hummer_early_prints, live since v18).  So the
// three functions are implemented here against that renderer instead of
// pulling in the whole boot-progress layer:
//
//   status    -> one "stage7: <detail>" line on the panel.  The renderer is a
//                fixed-size text surface, and the display path calls status on
//                every copied frame, so the first 48 lines are printed and the
//                rest is counted silently (a 60 Hz cadence would otherwise
//                scroll the bring-up lines away within a second).
//   handoff   -> always ready: hummer has no boot-progress framebuffer writer
//                to yield to the display driver.  The renderer writes through
//                its own block alias of the panel window, so the first full
//                system frame simply paints over the old text.
//   failed    -> nothing to resume (same reason); kept as a no-op so the
//                driver's failure path stays intact.
// The panel renderer's prototypes, spelled out here instead of including
// <arm64/hummer_early.h>: that header is written by build-hummer-console-probe.py,
// which runs later in the board's archive list than this builder, so on a fresh
// tree the include would not resolve yet (v69b lost a run to exactly that).
// The two signatures are the header's own; they have been stable since v18.
#include <stdint.h>   // uint64_t: v69c dropped the pexpert include that provided it
extern "C" {
void hummer_early_prints(const char *label, const char *value);
void hummer_early_u32x(const char *label, unsigned int value);

void
boot_progress_display_status(const char *detail, uint64_t value)
{
	static unsigned int reported;

	if (reported < 48) {
		reported++;
		hummer_early_prints("stage7", detail);
		if (value && reported < 48) {
			hummer_early_u32x("stage7", (unsigned int) value);
		}
	}
}

int
boot_progress_display_handoff(void)
{
	return 1;
}

void
boot_progress_display_failed(void)
{
}

} // extern "C"

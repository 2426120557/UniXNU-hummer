/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * hummer: GMU bring-up probe (Adreno 830 / gen8_0_0).
 *
 * What the register probes already established on this board:
 *   v145  the CX power domain comes up by clearing GPUCC_CX_GDSCR bit 0,
 *   v146  GPUCC PLL0 locks at 1 GHz,
 *   v147  the GMU is cold: CM3 held in reset, no firmware, HFI down, GX off,
 *   v148  the GMU's DTCM is writable from the kernel.
 *
 * This probe takes the next step: build the GMU's own SMMU context, map its
 * payloads at the addresses a6xx_gmu.c uses, load the pinned gen80000_gmu.bin,
 * release the CM3 and then run the full gen8 HFI startup.
 *
 * v184 replaces the hand-written message sequence of v163..v169 with the SM8750
 * GMU layer and the A8XX performance table from the OnePlus 13T port
 * (IssacL04/UniXNU-Pagani_SM8750), which drives the same SM8750/Adreno 830 to a
 * working Metal desktop.  It sits on top of v171..v183 (the USB/SMMU
 * diagnostics and the panic-mode report), which are unchanged below.  The hand-written version could only send a table whose
 * votes were all zero: the firmware acknowledged every message, left GX powered
 * off (PWR_CLK_STATUS stayed 3) and then rejected DCVS with 0x93.  GX is powered
 * from votes, so Sm8750GmuColdStart now feeds it the command-db ARC/BXM votes of
 * the real OPP table -- HummerGpuVotes plus AdrenoHfiGen8Votes (gen8 moved the
 * performance table to message 15 and added a dependent vote per level).
 *
 * The probe still stops after the GMU conversation and panics with the whole
 * state, so one boot shows whether GX came up.  CP/SQE and ZAP are the next step.
 *
 * Why it runs from native_legacy_display_late_probe() instead of the bsd_init
 * hook: the context bring-up allocates real memory through
 * IOBufferMemoryDescriptor, and IOKitBSDInit() has not run at the bsd_init hook
 * (bsd_init.c returns it from bsd_autoconf, long after vfsinit). The display
 * late-probe is the board's existing post-IOKit entry point, so no XNU source has
 * to be patched for this.
 *
 * Gated by hummer-adreno-bringup=1.  Bank/ASID/SMR slot/stream id default to the
 * values the device tree implies (GMU stream 5) but can be overridden with
 * hummer-adreno-bank / -asid / -slot / -sid, because which bank and slot are free
 * is a property of what the boot loader left behind, not of this source.
 */
#include <IOKit/IOLib.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <arm/machine_routines.h>

#include "Sm8750Gmu.h"
#include "Sm8750GmuSource.h"
#include "Sm8750GmuCommandDb.h"
#include "HummerGpuVotes.h"
#include "ShengGpuSmmuSource.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoKernelTime.h"
#include "NativeAdrenoSha256.h"
/* v167: the USB CDC console module's status block, so this probe can report why
 * the console never enumerated (see the panic at the end of the probe). */
#include "QcomXnuUsbDebug.h"

/* This translation unit is C++, so the kernel's C entry points must be declared
 * with C linkage or the reference is emitted mangled and the link fails with
 * "found _ml_io_map in machine_routines.o, declaration possibly missing
 * extern \"C\"" (that is exactly what v155 hit). */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" void boot_progress_stage(const char *s);

/* The firmware archive's strong definitions; NativeAdrenoFirmwareDefaults.cpp
 * only carries empty weak ones. */
extern "C" const uint8_t native_adreno_firmware_image[];
extern "C" const uint64_t native_adreno_firmware_image_size;

#define HU_GPUCC_BASE       0x3d90000ULL
#define HU_GPUCC_SIZE       0x10000ULL
#define HU_GPUCC_CX_GDSCR   0x9080u
#define HU_GPUCC_CX_HW_CTRL 0x9094u
#define HU_GDSC_SW_COLLAPSE (1u << 0)
#define HU_GDSC_HW_CONTROL  (1u << 1)

/* GPUCC clock branches. The annibale port (SM8750: same gpucc base 0x3d90000,
 * same offsets) refuses to start a GMU before its crystal branch (cxo) and the
 * GMU's own branch (cx_gmu) are both enabled and no longer report "off": with
 * the branch gated the CM3 has no clock and no firmware handshake can ever
 * complete. v159 only raised the CX power domain and never looked at these. */
#define HU_GPUCC_CX_CBCR       0x90e4u
#define HU_GPUCC_CX_GMU_CBCR   0x90d4u
#define HU_GPUCC_AHB_CBCR      0x90bcu
#define HU_GPUCC_MEMNOC_CBCR   0x90f4u
#define HU_CBCR_ENABLE         (1u << 0)
#define HU_CBCR_OFF            (1u << 31)

#define HU_SMMU_BASE  0x3da0000ULL
#define HU_SMMU_BYTES 0x40000ULL
/* CX_MISC is not one of the GMU's three kit windows; it is mapped here only for
 * the GX-rail observation (0x83*4 holds the GX power/clamp bits). */
#define HU_CX_MISC_BASE 0x3d9e000ULL
#define HU_CX_MISC_SIZE 0x2000ULL
/* GPUCC, SMEM and QFPROM windows the vote path needs.  QFPROM 0x138 carries the
 * 9-bit speed bin that selects an OPP table; SMEM is not read in this version. */
#define HU_QFPROM_BASE 0x221c8000ULL
#define HU_QFPROM_SIZE 0x1000ULL
#define HU_QFPROM_SPEED_BIN 0x138u
#define HU_CMDDb_BASE 0x81c60000ULL
#define HU_CMDDb_SIZE 0x20000ULL
/* GMU_PWR_CLK_STATUS: bit0 GX power, bit1 GX clock.  3 = both still off. */
#define HU_REG_GMU_PWR_CLK_STATUS 0x1f7e8u
/* DTCM's version word is the last word; the HFI wire version follows it by four
 * dwords (annibale's +16 is bytes).  Only used to report, not to decide. */
#define HU_REG_GMU_HFI_VERSION (REG_GMU_CM3_DTCM_START + 0xff8u + 4u)
/* The context bank's fault page, relative to the bank's base: FSR, FAR (low),
 * FSYNR0. FSR_FAULT clears every bit by writing ones. */
#define HU_CB_FSR     0x58u
#define HU_CB_FAR_LO  0x60u
#define HU_CB_FSYNR0  0x68u
#define HU_CB_FSR_FAULT 0xc00001feu

#define HU_RD(base, off)    (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#define HU_WR(base, off, v) (*(volatile uint32_t *)(uintptr_t)((base) + (off)) = (uint32_t)(v))

/* The GMU device keeps every resource it took, and allows one attempt per object,
 * so it has to outlive this call. */
static Sm8750GmuDevice hummer_gmu_device;

static int gmu_sha256(void *, const void *bytes, size_t length, uint8_t digest[32])
{
	NativeAdrenoSha256::hash(static_cast<const uint8_t *>(bytes), length, digest);
	return 0;
}

/* The context bring-up reports each step as it starts, so a hang shows up on the
 * panel instead of turning into a silent stop. */
static void ctx_progress(uint32_t step)
{
	switch (step) {
	case 1: boot_progress_stage("adreno-gmu: allocate"); break;
	case 2: boot_progress_stage("adreno-gmu: tables"); break;
	case 3: boot_progress_stage("adreno-gmu: program cb"); break;
	case 4: boot_progress_stage("adreno-gmu: attach streams"); break;
	case 5: boot_progress_stage("adreno-gmu: vm attach"); break;
	case 6: boot_progress_stage("adreno-gmu: map payloads"); break;
	case 7: boot_progress_stage("adreno-gmu: context ready"); break;
	default: break;
	}
}

/* CX power, exactly the sequence v145 verified on this board. */
static int cx_power_up(uint32_t *before, uint32_t *after)
{
	vm_offset_t gpucc = ml_io_map(HU_GPUCC_BASE, HU_GPUCC_SIZE);
	if (!gpucc) return 0;
	const uint32_t gdscr0 = HU_RD(gpucc, HU_GPUCC_CX_GDSCR);
	const uint32_t hw0 = HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL);
	if (before) *before = gdscr0;
	if (!((gdscr0 | hw0) >> 31) && !(gdscr0 & HU_GDSC_HW_CONTROL)) {
		HU_WR(gpucc, HU_GPUCC_CX_GDSCR, gdscr0 & ~HU_GDSC_SW_COLLAPSE);
		for (unsigned i = 0; i < 5000; ++i) {
			if ((HU_RD(gpucc, HU_GPUCC_CX_GDSCR) | HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL)) >> 31)
				break;
			IODelay(100);
		}
	}
	const uint32_t gdscr1 = HU_RD(gpucc, HU_GPUCC_CX_GDSCR);
	const uint32_t hw1 = HU_RD(gpucc, HU_GPUCC_CX_HW_CTRL);
	if (after) *after = gdscr1;
	return (gdscr1 | hw1) >> 31;
}

/* The GPU SMMU on this board is not virgin. The boot loader leaves stream routes
 * behind, and the arm-smmu kit refuses to attach a slot whose S2CR is not already
 * FAULT (ArmSmmu4KStreams.c: "All target S2CRs must already fault"). v156 proved
 * that matters here: slot 2 read back S2CR 0x03000018 (context 24, translate),
 * so the kit returned Busy and the bring-up stopped at step 4.
 *
 * The layout is the SMMU-500 one the kit and the K90 driver agree on: SMR at
 * 0x800+4n with bit 31 valid (bit 10 of S2CR instead in EXIDS mode), stream in
 * [14:0] and match mask in [30:16]; S2CR at 0xc00+4n with type in [17:16]
 * (0 = translate, 2 = fault) and the context bank in [7:0]. */
struct hu_smmu_slot {
	unsigned slot;
	uint32_t smr, s2cr;
};

struct hu_smmu_view {
	uint32_t id0, id1, id2, scr0;
	unsigned matches, exids, live, free_slots, pick, takeover, prefaulted;
	unsigned sid_taken;
	unsigned live_count;
	hu_smmu_slot live_map[4];
};

static void hu_smmu_scan(vm_offset_t base, unsigned sid, hu_smmu_view *v)
{
	*v = hu_smmu_view{};
	v->id0 = HU_RD(base, 0x20);
	v->id1 = HU_RD(base, 0x24);
	v->id2 = HU_RD(base, 0x28);
	v->scr0 = HU_RD(base, 0x00);
	v->matches = v->id0 & 0xffu;
	v->exids = (v->id0 >> 8) & 1u;
	v->pick = v->matches;      /* "nothing chosen" */
	v->takeover = v->matches;
	if (!v->matches)
		return;
	const uint32_t smr_valid = v->exids ? 0u : (1u << 31);
	const uint32_t s2cr_valid = v->exids ? (1u << 10) : 0u;
	for (unsigned i = 0; i < v->matches; ++i) {
		const uint32_t smr = HU_RD(base, 0x800 + 4 * i);
		const uint32_t s2cr = HU_RD(base, 0xc00 + 4 * i);
		if (!(smr & smr_valid)) {
			/* Not matching anything, so it is ours to take: an unclaimed
			 * SMR cannot conflict with SID 5 whatever its S2CR says. */
			if (v->takeover == v->matches)
				v->takeover = i;
			if ((s2cr & ~s2cr_valid) == 0x20000u) {
				++v->free_slots;
				if (v->pick == v->matches)
					v->pick = i;
			}
			continue;
		}
		++v->live;
		if (v->live_count < 4) {
			hu_smmu_slot *s = &v->live_map[v->live_count++];
			s->slot = i;
			s->smr = smr;
			s->s2cr = s2cr;
		}
		const uint32_t mask = (smr >> 16) & 0x7fffu;
		if (!((sid ^ (smr & 0x7fffu)) & ~mask & 0x7fffu))
			v->sid_taken = 1;
	}
}

/* True when a live translation route already names this context bank; the kit
 * rejects a candidate bank an active route points at. */
static int hu_bank_taken(const hu_smmu_view *v, unsigned bank)
{
	for (unsigned i = 0; i < v->live_count; ++i)
		if ((v->live_map[i].s2cr & 0x30000u) == 0 && (v->live_map[i].s2cr & 0xffu) == bank)
			return 1;
	return 0;
}

/* What the two clock branches and the GMU's context bank looked like, kept in
 * file scope so the final panic can print them: one boot shows one screen. */
static uint32_t hu_cxo_before, hu_cxo_after, hu_gmuclk_before, hu_gmuclk_after;
static uint32_t hu_ahb_after, hu_memnoc_after, hu_cxo_enabled, hu_gmuclk_enabled;
static uint32_t hu_cb_offset, hu_cb_fsr, hu_cb_far, hu_cb_syn, hu_cb_sctlr;

/* Turn a GPUCC branch on if it is not already running, the way the annibale
 * port's adreno8_clock_on does: read-modify-write bit 0, then wait for bit 31
 * to fall. Returns the register after the attempt. */
static uint32_t hu_branch_on(vm_offset_t gpucc, unsigned offset, uint32_t *changed)
{
	const uint32_t before = HU_RD(gpucc, offset);
	if ((before & HU_CBCR_ENABLE) && !(before & HU_CBCR_OFF))
		return before;
	HU_WR(gpucc, offset, before | HU_CBCR_ENABLE);
	uint32_t value = before;
	for (unsigned i = 0; i < 5000; ++i) {
		value = HU_RD(gpucc, offset);
		if (!(value & HU_CBCR_OFF))
			break;
		IODelay(10);
	}
	if (changed)
		*changed = 1;
	return value;
}

/* The A830 power sequence opens a set of GPUCC gates between "CX and PLL0 are
 * up" and "the GMU may start": six critical branches that are plain enables, the
 * CX NoC branch, and six clock branches that must also lose CLK_OFF.
 * Sm8750GpuPowerOn does exactly this (stage 5/6 of the OnePlus 13T port), and
 * their Run 75 stopped at GPUCC +0x9010 before that was corrected, so a gate
 * left closed here is a plausible reason for GX never to come up.  cx_power_up()
 * and PLL0 are already handled above; this only closes the remaining gates and
 * reports which ones it had to touch.  RCGR dividers are deliberately not
 * reprogrammed: the boot loader already clocks the GMU well enough for the
 * firmware to start (v160). */
static uint32_t hu_gpu_clock_gates(vm_offset_t gpucc)
{
	static const uint32_t critical[] = { 0x93a4, 0x9008, 0x9064, 0x93a8, 0x9004, 0x90cc };
	uint32_t changed = 0;
	for (unsigned i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i) {
		const uint32_t before = HU_RD(gpucc, critical[i]);
		if (!(before & HU_CBCR_ENABLE)) {
			HU_WR(gpucc, critical[i], before | HU_CBCR_ENABLE);
			changed |= 1u << i;
		}
	}
	IODelay(10);
	(void)hu_branch_on(gpucc, 0x90bcu, nullptr);
	static const uint32_t clocks[] = { 0x9010, 0x90d4, 0x90e4, 0x93e8, 0x90e8, 0x7000 };
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
		uint32_t enabled = 0;
		const uint32_t before = HU_RD(gpucc, clocks[i]);
		(void)hu_branch_on(gpucc, clocks[i], &enabled);
		if (enabled || !(before & HU_CBCR_ENABLE))
			changed |= 1u << (8 + i);
	}
	return changed;
}

/* Which payloads the CPU sees through their own (cacheable) kernel mapping. The
 * io submap refused the 16 MiB icache on v157 (kmem_alloc failed with KERN_NO_SPACE
 * inside ml_io_map), so that one falls back to NativeAdrenoGmuPayload.cpu and has to
 * be flushed before the CM3 is released: the GMU reads the buffers through its SMMU
 * as Normal non-cacheable. */
static uintptr_t gmu_cacheable_view[5];
static size_t gmu_cacheable_bytes[5];

/* Clean+invalidate every cacheable payload view, then wait for the maintenance to
 * complete.  This is the kit's `io.barrier` hook: Sm8750Gmu writes the icache,
 * calls barrier(), and only then releases the CM3, so cleaning here is what makes
 * the GMU see the words the CPU just stored.  Only ranges reached through a
 * cacheable mapping are touched -- cache maintenance on an uncached alias is a
 * no-op at best.  The memory clobber stops the compiler sinking a store past the
 * instruction that cleans its line. */
static int hu_gmu_barrier(void *)
{
	for (unsigned i = 0; i < 5; ++i) {
		if (!gmu_cacheable_view[i] || !gmu_cacheable_bytes[i])
			continue;
		const uintptr_t start = gmu_cacheable_view[i];
		const uintptr_t end =
		    (start + gmu_cacheable_bytes[i] + 63) & ~static_cast<uintptr_t>(63);
		for (uintptr_t line = start; line < end; line += 64)
			__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
	}
	__asm__ volatile("dsb sy" : : : "memory");
	return 0;
}

extern "C" void hummer_adreno_bringup_probe(void)
{
	uint32_t enabled = 0;
	if (!PE_parse_boot_argn("hummer-adreno-bringup", &enabled, sizeof(enabled)) || !enabled)
		return;

	uint32_t cx_before = 0, cx_after = 0;
	boot_progress_stage("adreno-gmu: cx");
	if (!cx_power_up(&cx_before, &cx_after))
		panic("hummer gmu: CX did not come up gdscr=%08x -> %08x", cx_before, cx_after);

	/* CX power alone does not clock the GMU. Both branches are read, and each
	 * one that is gated is enabled here; the annibale port treats a branch that
	 * stays off as a reason to stop before touching the firmware. */
	vm_offset_t gpucc = ml_io_map(HU_GPUCC_BASE, HU_GPUCC_SIZE);
	if (!gpucc)
		panic("hummer gmu: gpucc unmappable");
	hu_cxo_before = HU_RD(gpucc, HU_GPUCC_CX_CBCR);
	hu_gmuclk_before = HU_RD(gpucc, HU_GPUCC_CX_GMU_CBCR);
	hu_cxo_after = hu_branch_on(gpucc, HU_GPUCC_CX_CBCR, &hu_cxo_enabled);
	hu_gmuclk_after = hu_branch_on(gpucc, HU_GPUCC_CX_GMU_CBCR, &hu_gmuclk_enabled);
	hu_ahb_after = HU_RD(gpucc, HU_GPUCC_AHB_CBCR);
	hu_memnoc_after = HU_RD(gpucc, HU_GPUCC_MEMNOC_CBCR);
	boot_progress_stage("adreno-gmu: clocks");

	boot_progress_stage("adreno-gmu: mmio");
	vm_offset_t gmu = ml_io_map(sm8750_gmu_window_physical[Sm8750GmuCore],
	                            sm8750_gmu_window_bytes[Sm8750GmuCore]);
	vm_offset_t rscc = ml_io_map(sm8750_gmu_window_physical[Sm8750GmuRscc],
	                             sm8750_gmu_window_bytes[Sm8750GmuRscc]);
	vm_offset_t gpu = ml_io_map(sm8750_gmu_window_physical[Sm8750GmuGpu],
	                            sm8750_gmu_window_bytes[Sm8750GmuGpu]);
	vm_offset_t cx_misc = ml_io_map(HU_CX_MISC_BASE, HU_CX_MISC_SIZE);
	vm_offset_t smmu = ml_io_map(HU_SMMU_BASE, HU_SMMU_BYTES);
	if (!gmu || !rscc || !gpu || !cx_misc || !smmu)
		panic("hummer gmu: window unmappable gmu=%lx rscc=%lx gpu=%lx cxmisc=%lx smmu=%lx",
		    (unsigned long)gmu, (unsigned long)rscc, (unsigned long)gpu,
		    (unsigned long)cx_misc, (unsigned long)smmu);

	/* Sm8750GmuBindMmio validates physical base and size against its own pinned
	 * DT table, so the windows are handed over unchanged; CX_MISC stays outside
	 * the kit (it is observation only). */
	Sm8750GmuMmio mmio{};
	struct { unsigned window; vm_offset_t cpu; } bound[] = {
		{ Sm8750GmuCore, gmu }, { Sm8750GmuRscc, rscc },
		{ Sm8750GmuGpu, gpu },
	};
	for (unsigned i = 0; i < sizeof(bound) / sizeof(bound[0]); ++i) {
		Sm8750GmuMmioWindow &w = mmio.windows[bound[i].window];
		w.cpu = reinterpret_cast<volatile unsigned char *>(bound[i].cpu);
		w.physical = sm8750_gmu_window_physical[bound[i].window];
		w.bytes = sm8750_gmu_window_bytes[bound[i].window];
	}

	Sm8750GmuIo io{};
	if (Sm8750GmuBindMmio(&mmio, &io) != Sm8750GmuSuccess)
		panic("hummer gmu: Sm8750GmuBindMmio refused the DT windows");
	/* Replace the kit's dsb-only barrier with one that also cleans the cacheable
	 * payload views: Sm8750Gmu releases the CM3 right after a barrier. */
	io.barrier = hu_gmu_barrier;

	/* No default slot: v156 proved slot 2 carries a boot-loader route, so "pick
	 * the first slot that is genuinely free" has to be the default and an explicit
	 * hummer-adreno-slot overrides it. */
	uint32_t bank_arg = ~0u, asid = 1, slot_arg = ~0u, sid = SHENG_GPU_SMMU_GMU_SID;
	(void)PE_parse_boot_argn("hummer-adreno-bank", &bank_arg, sizeof(bank_arg));
	(void)PE_parse_boot_argn("hummer-adreno-asid", &asid, sizeof(asid));
	(void)PE_parse_boot_argn("hummer-adreno-slot", &slot_arg, sizeof(slot_arg));
	(void)PE_parse_boot_argn("hummer-adreno-sid", &sid, sizeof(sid));

	hu_smmu_view scan;
	hu_smmu_scan(smmu, sid, &scan);
	if (scan.sid_taken)
		panic("hummer gmu: SID %u already routed by the boot loader "
		      "id0=%08x matches=%u live=%u free=%u\n"
		      "lu0 %u %08x/%08x  lu1 %u %08x/%08x  lu2 %u %08x/%08x  lu3 %u %08x/%08x",
		    sid, scan.id0, scan.matches, scan.live, scan.free_slots,
		    scan.live_map[0].slot, scan.live_map[0].smr, scan.live_map[0].s2cr,
		    scan.live_map[1].slot, scan.live_map[1].smr, scan.live_map[1].s2cr,
		    scan.live_map[2].slot, scan.live_map[2].smr, scan.live_map[2].s2cr,
		    scan.live_map[3].slot, scan.live_map[3].smr, scan.live_map[3].s2cr);

	unsigned bank = bank_arg;
	if (bank_arg == ~0u) {
		bank = 2;                      /* v156 proved bank 2 accepts the image */
		while (bank < 64 && hu_bank_taken(&scan, bank))
			++bank;
		if (bank >= 64)
			bank = 2;                  /* let the kit report it instead of guessing */
	}

	unsigned slot;
	if (slot_arg != ~0u) {
		slot = slot_arg;
	} else if (scan.pick < scan.matches) {
		slot = scan.pick;
	} else if (scan.takeover < scan.matches) {
		slot = scan.takeover;
	} else {
		panic("hummer gmu: no SMMU slot to route id0=%08x matches=%u live=%u free=%u",
		    scan.id0, scan.matches, scan.live, scan.free_slots);
	}

	/* The kit requires the target slot to already read FAULT. A slot whose SMR is
	 * invalid matches nothing, so faulting it first cannot disturb a live device,
	 * and it is the only way past a leftover S2CR such as v156 found. */
	if (slot < scan.matches) {
		const uint32_t s2cr = HU_RD(smmu, 0xc00 + 4 * slot);
		if ((s2cr & ~0x400u) != 0x20000u) {
			HU_WR(smmu, 0xc00 + 4 * slot, 0x20000u);
			(void)HU_RD(smmu, 0xc00 + 4 * slot);
			scan.prefaulted = 1;
		}
	}
	if (slot > 127)
		panic("hummer gmu: slot %u beyond the kit's limit id0=%08x matches=%u",
		    slot, scan.id0, scan.matches);

	boot_progress_stage("adreno-gmu: smmu ctx");
	static NativeAdrenoGmuContextReport report;
	const IOReturn context_status = NativeAdrenoGmuContextBringup(
	    reinterpret_cast<volatile uint8_t *>(smmu), HU_SMMU_BYTES,
	    bank, asid, slot, sid, ctx_progress, &report);

	static NativeAdrenoGmuPayload payloads[5];
	const IOReturn payload_status = NativeAdrenoGmuContextPayloads(payloads);
	if (context_status || payload_status)
		panic("hummer gmu: ctx failed status=%x step=%u inner=%x payload=%x "
		      "kit=%u phase=%u w=%u vfy=%u\n"
		      "smmu id0=%08x id1=%08x scr0=%08x matches=%u exids=%u live=%u free=%u sidtaken=%u\n"
		      "pick slot=%u bank=%u sid=%u prefault=%u bktaken=%u smr=%08x s2cr=%08x "
		      "sctlr=%08x ttbr0=%llx\n"
		      "lu0 %u %08x/%08x  lu1 %u %08x/%08x\n"
		      "lu2 %u %08x/%08x  lu3 %u %08x/%08x",
		    context_status, report.step, report.status, payload_status,
		    report.route_status, report.route_phase, report.route_writes, report.routes_verified,
		    scan.id0, scan.id1, scan.scr0, scan.matches, scan.exids, scan.live, scan.free_slots,
		    scan.sid_taken,
		    slot, bank, sid, scan.prefaulted, hu_bank_taken(&scan, bank),
		    report.smr, report.s2cr, report.sctlr, (unsigned long long)report.ttbr0,
		    scan.live_map[0].slot, scan.live_map[0].smr, scan.live_map[0].s2cr,
		    scan.live_map[1].slot, scan.live_map[1].smr, scan.live_map[1].s2cr,
		    scan.live_map[2].slot, scan.live_map[2].smr, scan.live_map[2].s2cr,
		    scan.live_map[3].slot, scan.live_map[3].smr, scan.live_map[3].s2cr);

	/* The context bank's fault registers are the only place a failed fetch by the
	 * GMU shows up: the GMU's own status words stayed clean on v159 while the
	 * firmware never started. The annibale port saw CB2 FSR=0x8 / FAR=0x4000 /
	 * FSYNR0=0xd0063 there -- a privileged instruction read denied on the very
	 * first page of the icache -- and needed the leaf PTEs' AP[1] cleared to get
	 * past it. Clear the bank's faults before the load so whatever the panic
	 * prints belongs to this attempt. The bank's base is the kit's own layout:
	 * (pages + bank) * stride, both from IDR1. */
	const unsigned cb_shift = (scan.id1 >> 31) ? 16 : 12;
	const uint64_t cb_pages = 1ull << (((scan.id1 >> 28) & 7) + 1);
	const uint64_t cb_base = (cb_pages + bank) * (1ull << cb_shift);
	if (cb_base + 0x7f8 <= HU_SMMU_BYTES) {
		hu_cb_offset = static_cast<uint32_t>(cb_base);
		HU_WR(smmu, hu_cb_offset + HU_CB_FSR, HU_CB_FSR_FAULT);
		(void)HU_RD(smmu, hu_cb_offset + HU_CB_FSR);
	}

	/* --- v184: the SM8750 GMU layer and a real OPP table. ---------------------
	 * v163..v169 hand-built the HFI messages.  Every one of 15/11/14/10 was
	 * acknowledged, yet GMU_PWR_CLK_STATUS stayed 3 (GX power and clock off) and
	 * DCVS (30) came back 0x93: the performance table those messages carried had
	 * vote=0 and dep=0 on every level, because there was no command-db reader to
	 * supply them.  GX is powered from the votes in that table, so this version
	 * derives them from the real OPP list instead of guessing.
	 *
	 * The layer, the A8XX table encoder and the OPP policy are the ones the
	 * OnePlus 13T port uses (IssacL04/UniXNU-Pagani_SM8750) -- same SM8750, same
	 * Adreno 830, and it reaches a Metal desktop there. */
	Sm8750GmuConfig config{};
	config.chip_id = SM8750_GMU_CHIP_ID;
	config.firmware = native_adreno_firmware_image;
	config.firmware_bytes = static_cast<size_t>(native_adreno_firmware_image_size);
	config.poll_budget = 1000;
	Sm8750GmuMemory *memory[5] = { &config.icache, &config.dummy, &config.debug,
	                               &config.log, &config.hfi };
	boot_progress_stage("adreno-gmu: payload views");
	/* ml_io_map() does not return 0 when the io submap cannot serve a request: it
	 * reaches kmem_alloc(), which panic()s outright ("kmem_alloc(...): failed with
	 * 3 @vm_kern.c:180"). v157 and v158 both died there on the 16 MiB icache before
	 * any fallback could run, so the request is never made at all. Only sizes this
	 * board has already mapped are handed to it (the GPU windows above are up to
	 * 0x68000); everything larger uses the payload's own kernel mapping, which
	 * hu_gmu_barrier() cleans before the CM3 is released. */
	static const uint64_t kUncachedCeiling = 0x100000;
	unsigned uncached = 0, cacheable = 0;
	for (unsigned i = 0; i < 5; ++i) {
		vm_offset_t view = 0;
		if (payloads[i].bytes <= kUncachedCeiling)
			view = ml_io_map(payloads[i].physical,
			                 static_cast<vm_size_t>(payloads[i].bytes));
		if (view) {
			++uncached;
		} else if (payloads[i].cpu) {
			view = static_cast<vm_offset_t>(payloads[i].cpu);
			gmu_cacheable_view[i] = static_cast<uintptr_t>(view);
			gmu_cacheable_bytes[i] = static_cast<size_t>(payloads[i].bytes);
			++cacheable;
		}
		if (!view)
			panic("hummer gmu: payload %u unmappable pa=%llx bytes=%llx cpu=%llx", i,
			    (unsigned long long)payloads[i].physical,
			    (unsigned long long)payloads[i].bytes,
			    (unsigned long long)payloads[i].cpu);
		memory[i]->cpu = reinterpret_cast<volatile uint32_t *>(view);
		memory[i]->bytes = static_cast<size_t>(payloads[i].bytes);
		memory[i]->iova = payloads[i].iova;
	}

	Sm8750GmuServices services{};
	services.sha256 = gmu_sha256;
	services.now_us = NativeAdrenoKernelTime::now_us;
	services.wait_us = NativeAdrenoKernelTime::wait_us;

	hummer_gmu_device = Sm8750GmuDevice{};
	hummer_gmu_device.io = io;
	hummer_gmu_device.services = services;

	/* --- votes ---------------------------------------------------------------
	 * The command-db snapshot is a fixed 0x20000 window at 0x81c60000; the parser
	 * refuses anything smaller because every offset in the header is relative to
	 * the end of that window.  QFPROM 0x138 holds the 9-bit speed bin that picks
	 * the OPP profile.  If that word is not one of the bins the tables were
	 * generated from, each known bin is tried in turn and the substitution is
	 * printed: the top OPPs differ per bin, but every profile starts at 160 MHz
	 * and this bring-up asks for 525 MHz at most. */
	boot_progress_stage("adreno-gmu: votes");
	vm_offset_t cmddb = ml_io_map(HU_CMDDb_BASE, HU_CMDDb_SIZE);
	vm_offset_t qfprom = ml_io_map(HU_QFPROM_BASE, HU_QFPROM_SIZE);
	if (!cmddb || !qfprom)
		panic("hummer gmu: vote windows unmappable cmddb=%lx qfprom=%lx",
		    (unsigned long)cmddb, (unsigned long)qfprom);

	Sm8750GmuVoteInputs votes{};
	const Sm8750GmuDbStatus db_status = Sm8750GmuCommandDbInputs(
	    reinterpret_cast<const uint8_t *>(cmddb), HU_CMDDb_SIZE, &votes);
	const uint32_t speed_raw = HU_RD(qfprom, HU_QFPROM_SPEED_BIN) & 0x1ffu;
	uint32_t speed_used = speed_raw, speed_fallback = 0;
	AdrenoHfiStartupConfig startup{};
	unsigned initial = 0, bandwidth = 0;
	AdrenoHfiStartupStatus vote_status = AdrenoHfiStartupInvalid;
	if (db_status == Sm8750GmuDbSuccess) {
		vote_status = HummerGpuBuildStartup(&votes, speed_raw, 0, &startup,
		                                    &initial, &bandwidth);
		static const uint32_t known_bins[] = { 190, 221, 232, 242 };
		for (unsigned i = 0;
		     i < sizeof(known_bins) / sizeof(known_bins[0]) &&
		     vote_status != AdrenoHfiStartupSuccess; ++i) {
			vote_status = HummerGpuBuildStartup(&votes, known_bins[i], 0, &startup,
			                                    &initial, &bandwidth);
			if (vote_status == AdrenoHfiStartupSuccess) {
				speed_used = known_bins[i];
				speed_fallback = 1;
			}
		}
	}
	if (db_status != Sm8750GmuDbSuccess || vote_status != AdrenoHfiStartupSuccess)
		panic("hummer gmu: votes refused db=%u bin=%u(%u) status=%u "
		      "gfx=%u mx=%u cx=%u gmxc=%u gbx=%u\n"
		      "sh0=%x mc0=%x acv=%x cn0=%x cxad=%x mxad=%x",
		    db_status, speed_raw, speed_used, vote_status,
		    (unsigned)votes.gfx.bytes, (unsigned)votes.mx.bytes,
		    (unsigned)votes.cx.bytes, (unsigned)votes.gmxc.bytes,
		    (unsigned)votes.gbx.bytes,
		    votes.sh0.address, votes.mc0.address, votes.acv.address,
		    votes.cn0_address, votes.cx_address, votes.mx_address);

	const uint32_t pwr_before = HU_RD(gmu, (HU_REG_GMU_PWR_CLK_STATUS << 2) -
	                                       SM8750_GMU_CORE_GPU_DELTA);

	/* Close the remaining GPUCC gates before anything asks GX to power up. */
	boot_progress_stage("adreno-gmu: gates");
	const uint32_t gates_changed = hu_gpu_clock_gates(gpucc);

	/* --- cold start: firmware + the whole gen8 HFI startup -------------------
	 * Sm8750GmuColdStart validates the pinned image, writes and reads back all
	 * 15 records, releases the CM3 and then runs AdrenoHfiStartupRun, which sends
	 * the A8XX performance table (message 15), the bandwidth table, CORE_FW_START
	 * and START, waiting for a real ACK on each.  A protocol failure resets and
	 * poisons the device; every allocation is retained either way. */
	static const Sm8750GmuStartupLimits limits = { 1000, 100, 100000, 2000000 };
	static Sm8750GmuStartupResult cold{};
	boot_progress_stage("adreno-gmu: cold start");
	const Sm8750GmuStatus cold_status = Sm8750GmuColdStart(
	    &hummer_gmu_device, &config, &startup, &limits, &cold);

	/* GX comes up while the firmware places its own requests, so the vote is only
	 * sent when startup succeeded and then given its own, longer poll window. */
	static Sm8750GmuResult runtime{};
	Sm8750GmuStatus dcvs_status = Sm8750GmuStateMismatch;
	Sm8750GmuStatus oob_status = Sm8750GmuStateMismatch;
	if (cold_status == Sm8750GmuSuccess) {
		boot_progress_stage("adreno-gmu: dcvs");
		dcvs_status = Sm8750GmuSetPerformance(&hummer_gmu_device, initial,
		                                      bandwidth, &runtime);
		boot_progress_stage("adreno-gmu: oob");
		oob_status = Sm8750GmuGpuOob(&hummer_gmu_device, 1, &runtime);
	}
	boot_progress_stage("adreno-gmu: read back");

	auto gmu_word = [gmu](uint32_t word) {
		return HU_RD(gmu, (word << 2) - SM8750_GMU_CORE_GPU_DELTA);
	};
	const uint32_t pwr_after = gmu_word(HU_REG_GMU_PWR_CLK_STATUS);
	const uint32_t gx_misc = HU_RD(cx_misc, 0x83u * 4u);
	const uint32_t hfi_version = gmu_word(HU_REG_GMU_HFI_VERSION);
	const uint32_t hfi_ctrl = gmu_word(REG_GMU_HFI_CTRL_STATUS);
	const uint32_t log_written = gmu_word(REG_GMU_GENERAL_9) & 0xffffu;
	uint32_t log_words[2] = {};
	const volatile uint32_t *log =
	    reinterpret_cast<const volatile uint32_t *>(payloads[3].cpu);
	if (log && log_written)
		for (unsigned i = 0; i < 2; ++i)
			log_words[i] = log[i];

	if (hu_cb_offset) {
		hu_cb_sctlr = HU_RD(smmu, hu_cb_offset);
		hu_cb_fsr = HU_RD(smmu, hu_cb_offset + HU_CB_FSR);
		hu_cb_far = HU_RD(smmu, hu_cb_offset + HU_CB_FAR_LO);
		hu_cb_syn = HU_RD(smmu, hu_cb_offset + HU_CB_FSYNR0);
	}

	/* --- v167: why did the USB CDC console not enumerate? ---------------------
	 * v166 built and linked the console (the module's symbols resolved), the boot
	 * arg is in UNIXNU.TOM, yet no COM port ever appeared. qcom_usb_debug_init()
	 * runs from PE_init_kprintf -- far earlier than this probe -- and on failure
	 * it only records why in its own status block. Nothing prints that, and a
	 * failed init is silent by design (it must not disturb boot). So the probe
	 * that already panics once per boot asks the module what happened. */
	static qcom_usb_debug_status usb{};
	qcom_usb_debug_get_status(&usb);
	uint32_t usb_arg = 0;
	(void)PE_parse_boot_argn("qcom-usb-debug", &usb_arg, sizeof(usb_arg));
	/* If the early call refused, try once from here: this probe runs long after
	 * CPU data, the scheduler and IOKit exist, so a different answer separates
	 * "wrong moment" from "wrong hardware state". */
	const uint32_t usb_phase0 = usb.phase;
	int usb_retry = 0;
	if (usb.phase == QCOM_USB_DEBUG_NOT_INITIALIZED) {
		usb_retry = qcom_usb_debug_init();
		qcom_usb_debug_get_status(&usb);
	}

	/* v171: both DMA backends die on the same two bits (ID0 bit30, ID2 bit12),
	 * one reporting Unsupported and the other UnknownSmmu. That is a statement
	 * about what 0x15000000 answers, not about routing, so read the raw geometry
	 * here instead of inferring it: SMMUv2 puts sCR0 at 0x00, ID0..ID2 at
	 * 0x20..0x28 and sACR at 0x30. All-zero means the block was not clocked. */
	vm_offset_t apps_smmu = ml_io_map(0x15000000ULL, 0x1000ULL);
	uint32_t sid0 = 0, sid1 = 0, sid2 = 0, sscr0 = 0, ssacr = 0;
	if (apps_smmu) {
		sid0 = HU_RD(apps_smmu, 0x20u);
		sid1 = HU_RD(apps_smmu, 0x24u);
		sid2 = HU_RD(apps_smmu, 0x28u);
		sscr0 = HU_RD(apps_smmu, 0x00u);
		ssacr = HU_RD(apps_smmu, 0x30u);
	}
	/* The same numbers the bypass planner derives, computed here so a stale
	 * module object (old 128-entry limit) is visible as "planner should have
	 * accepted this" instead of being mistaken for another SMMU rejection. */
	const uint32_t sstreams = sid0 & 255u;
	const uint32_t sbanks = sid1 & 255u;
	const uint32_t spages = 1u << (((sid1 >> 28) & 7u) + 1u);

	/* v179: kprintf routes through _doprnt_log, whose encoder asserts that a
	 * single literal argument fits in a command (arg_size <= UINT8_MAX). The full
	 * report below is ~800 bytes, so printing it in one call panics inside
	 * libkern/os/log_encode.c. Each line below is well under that limit. */
	uint32_t want_panic = 0;
	(void)PE_parse_boot_argn("hummer-adreno-panic", &want_panic, sizeof(want_panic));

	if (want_panic)
		panic("hummer gmu: cold=%u proto=%u stage=%u xport=%u fwerr=%08x cmds=%u polls=%u\n"
		      "load phase=%u fw=%08x hfi=%08x hash=%u reset=%u ao=%08x cm3=%08x uc=%u cc=%u\n"
		      "dcvs=%u oob=%u pwr=%08x->%08x gx=%08x hfiver=%08x ctrl=%08x\n"
		      "votes db=%u bin=%u(%u,%u) gpu=%u gmu=%u bw=%u cnoc=%u ddr=%u\n"
		      "perf init=%u mhz=%u v0=%08x vi=%08x di=%08x\n"
		      "cb fsr=%08x far=%08x syn=%08x sctlr=%08x off=%x\n"
		      "clk cxo=%08x/%08x gmu=%08x/%08x ahb=%08x noc=%08x gates=%02x\n"
		      "ctx bank=%u asid=%u slot=%u sid=%u spans=%u ttbr0=%llx\n"
		      "log=%u %08x %08x\n"
		      "usb arg=%u p0=%u ri=%d ph=%u er=%u qr=%u\n"
		      "usb2 dr=%u ds=%u bk=%u ta=%x tb=%x ev=%x\n"
		      "smmu id0=%08x id1=%08x id2=%08x scr0=%08x sacr=%08x\n"
		      "smmu2 str=%u bk=%u pg=%u cr0b0=%u tag=%s\n"
		      "iou: %llx %llx %llx %llx %llx",
		    (unsigned)cold_status, (unsigned)cold.protocol_status,
		    (unsigned)cold.protocol.stage, (unsigned)cold.protocol.transport_status,
		    cold.protocol.firmware_error, cold.protocol.published_commands,
		    cold.protocol.poll_reads,
		    cold.load.phase, cold.load.firmware_init_result, cold.load.hfi_status,
		    cold.load.complete_hash_verified, cold.load.reset_verified,
		    cold.load.ao_fault_status, cold.load.cm3_fault_status, uncached, cacheable,
		    (unsigned)dcvs_status, (unsigned)oob_status, pwr_before, pwr_after, gx_misc,
		    hfi_version, hfi_ctrl,
		    (unsigned)db_status, speed_raw, speed_used, speed_fallback,
		    startup.gpu_count, startup.gmu_count, startup.bandwidth_count,
		    startup.cnoc_count, startup.ddr_count,
		    initial, (unsigned)(startup.gpu[initial].hz / 1000000u),
		    startup.gpu[0].vote, startup.gpu[initial].vote,
		    startup.gpu_dependency_votes[initial],
		    hu_cb_fsr, hu_cb_far, hu_cb_syn, hu_cb_sctlr, hu_cb_offset,
		    hu_cxo_before, hu_cxo_after, hu_gmuclk_before, hu_gmuclk_after,
		    hu_ahb_after, hu_memnoc_after, gates_changed,
		    bank, asid, slot, sid, report.spans, (unsigned long long)report.ttbr0,
		    log_written, log_words[0], log_words[1],
		    usb_arg, usb_phase0, usb_retry, usb.phase, usb.error, usb.qcom_result,
		    usb.domain_result, usb.domain_stage, usb.domain_bank,
		    usb.trace_a, usb.trace_b, usb.last_poll_events,
		    sid0, sid1, sid2, sscr0, ssacr,
		    sstreams, sbanks, spages, sscr0 & 1u, "v184-256",
		    (unsigned long long)payloads[0].iova, (unsigned long long)payloads[1].iova,
		    (unsigned long long)payloads[2].iova, (unsigned long long)payloads[3].iova,
		    (unsigned long long)payloads[4].iova);
	else {
		/* One kprintf per line: every literal here stays far below the log
		 * encoder's per-command limit, unlike the ~800-byte block above. */
		kprintf("hummer gmu: cold=%u proto=%u stage=%u xport=%u fwerr=%08x cmds=%u\n",
		    (unsigned)cold_status, (unsigned)cold.protocol_status,
		    (unsigned)cold.protocol.stage, (unsigned)cold.protocol.transport_status,
		    cold.protocol.firmware_error, cold.protocol.published_commands);
		kprintf("load phase=%u fw=%08x hfi=%08x hash=%u reset=%u uc=%u cc=%u\n",
		    cold.load.phase, cold.load.firmware_init_result, cold.load.hfi_status,
		    cold.load.complete_hash_verified, cold.load.reset_verified,
		    uncached, cacheable);
		kprintf("dcvs=%u oob=%u pwr=%08x->%08x gx=%08x hfiver=%08x\n",
		    (unsigned)dcvs_status, (unsigned)oob_status, pwr_before, pwr_after,
		    gx_misc, hfi_version);
		kprintf("votes db=%u bin=%u(%u,%u) gpu=%u gmu=%u bw=%u cnoc=%u ddr=%u\n",
		    (unsigned)db_status, speed_raw, speed_used, speed_fallback,
		    startup.gpu_count, startup.gmu_count, startup.bandwidth_count,
		    startup.cnoc_count, startup.ddr_count);
		kprintf("perf init=%u mhz=%u v0=%08x vi=%08x di=%08x\n",
		    initial, (unsigned)(startup.gpu[initial].hz / 1000000u),
		    startup.gpu[0].vote, startup.gpu[initial].vote,
		    startup.gpu_dependency_votes[initial]);
		kprintf("clk cxo=%08x/%08x gmu=%08x/%08x noc=%08x gates=%02x\n",
		    hu_cxo_before, hu_cxo_after, hu_gmuclk_before, hu_gmuclk_after,
		    hu_memnoc_after, gates_changed);
		kprintf("cb fsr=%08x far=%08x syn=%08x sctlr=%08x off=%x\n",
		    hu_cb_fsr, hu_cb_far, hu_cb_syn, hu_cb_sctlr, hu_cb_offset);
		kprintf("ctx bank=%u asid=%u slot=%u sid=%u spans=%u ttbr0=%llx\n",
		    bank, asid, slot, sid, report.spans,
		    (unsigned long long)report.ttbr0);
		kprintf("usb arg=%u p0=%u ri=%d ph=%u er=%u qr=%u\n",
		    usb_arg, usb_phase0, usb_retry, usb.phase, usb.error, usb.qcom_result);
		kprintf("usb2 dr=%u ds=%u bk=%u ta=%x tb=%x ev=%x\n",
		    usb.domain_result, usb.domain_stage, usb.domain_bank,
		    usb.trace_a, usb.trace_b, usb.last_poll_events);
		kprintf("smmu id0=%08x id1=%08x id2=%08x scr0=%08x sacr=%08x\n",
		    sid0, sid1, sid2, sscr0, ssacr);
		kprintf("smmu2 str=%u bk=%u pg=%u cr0b0=%u tag=%s\n",
		    sstreams, sbanks, spages, sscr0 & 1u, "v184-256");
	}
}

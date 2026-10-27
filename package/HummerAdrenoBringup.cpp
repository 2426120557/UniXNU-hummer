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
 * payloads at the addresses a6xx_gmu.c uses, load the pinned gen80000_gmu.bin and
 * release the CM3.  It stops at "firmware signalled FW_INIT_RESULT and the HFI
 * controller reports ready" -- the last point reachable without the A8XX HFI
 * table format, which the pinned AdrenoFirmware kit does not implement.
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

#include "ShengGmu.h"
#include "ShengGmuSource.h"
#include "ShengGpuSmmuSource.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoKernelTime.h"
#include "NativeAdrenoSha256.h"

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
static ShengGmuDevice hummer_gmu_device;

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

/* Which payloads the CPU sees through their own (cacheable) kernel mapping. The
 * io submap refused the 16 MiB icache on v157 (kmem_alloc failed with KERN_NO_SPACE
 * inside ml_io_map), so that one falls back to NativeAdrenoGmuPayload.cpu and has to
 * be flushed before the CM3 is released: the GMU reads the buffers through its SMMU
 * as Normal non-cacheable. */
static uintptr_t gmu_cacheable_view[5];

/* Clean+invalidate the lines covering a range, then wait for the maintenance to
 * complete -- the order the A8XX reference uses before the other side reads a
 * buffer this kernel just built. The memory clobber stops the compiler from sinking
 * a store past the instruction that cleans its line. Only ranges reached through a
 * cacheable mapping are touched: cache maintenance on the uncached aliases would be
 * a no-op at best. */
static int gmu_flush(void *, const void *bytes, size_t length)
{
	const uintptr_t at = reinterpret_cast<uintptr_t>(bytes);
	int cacheable = 0;
	for (unsigned i = 0; i < 5; ++i)
		if (at == gmu_cacheable_view[i])
			cacheable = 1;
	if (!cacheable || !length)
		return 0;
	uintptr_t line = at & ~static_cast<uintptr_t>(63);
	const uintptr_t end = (at + length + 63) & ~static_cast<uintptr_t>(63);
	for (; line < end; line += 64)
		__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
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
	vm_offset_t gmu = ml_io_map(sheng_gmu_window_physical[ShengGmuCore],
	                            sheng_gmu_window_bytes[ShengGmuCore]);
	vm_offset_t rscc = ml_io_map(sheng_gmu_window_physical[ShengGmuRscc],
	                             sheng_gmu_window_bytes[ShengGmuRscc]);
	vm_offset_t gpu = ml_io_map(sheng_gmu_window_physical[ShengGmuGpu],
	                            sheng_gmu_window_bytes[ShengGmuGpu]);
	vm_offset_t cx_misc = ml_io_map(sheng_gmu_window_physical[ShengGmuCxMisc],
	                                sheng_gmu_window_bytes[ShengGmuCxMisc]);
	vm_offset_t smmu = ml_io_map(HU_SMMU_BASE, HU_SMMU_BYTES);
	if (!gmu || !rscc || !gpu || !cx_misc || !smmu)
		panic("hummer gmu: window unmappable gmu=%lx rscc=%lx gpu=%lx cxmisc=%lx smmu=%lx",
		    (unsigned long)gmu, (unsigned long)rscc, (unsigned long)gpu,
		    (unsigned long)cx_misc, (unsigned long)smmu);

	ShengGmuMmio mmio{};
	struct { unsigned window; vm_offset_t cpu; } bound[] = {
		{ ShengGmuCore, gmu }, { ShengGmuRscc, rscc },
		{ ShengGmuGpu, gpu }, { ShengGmuCxMisc, cx_misc },
	};
	for (unsigned i = 0; i < sizeof(bound) / sizeof(bound[0]); ++i) {
		ShengGmuMmioWindow &w = mmio.windows[bound[i].window];
		w.cpu = reinterpret_cast<volatile unsigned char *>(bound[i].cpu);
		w.physical = sheng_gmu_window_physical[bound[i].window];
		w.bytes = sheng_gmu_window_bytes[bound[i].window];
	}
	/* ShengGmuPdc stays absent on SM8750 and ShengGmuBindMmio requires exactly that. */

	ShengGmuIo io{};
	if (ShengGmuBindMmio(&mmio, &io) != ShengGmuSuccess)
		panic("hummer gmu: ShengGmuBindMmio refused the DT windows");

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

	ShengGmuConfig config{};
	config.chip_id = SHENG_GMU_CHIP_ID;
	config.firmware = native_adreno_firmware_image;
	config.firmware_bytes = static_cast<size_t>(native_adreno_firmware_image_size);
	config.poll_budget = 1000;
	ShengGmuMemory *memory[5] = { &config.icache, &config.dummy, &config.debug,
	                              &config.log, &config.hfi };
	boot_progress_stage("adreno-gmu: payload views");
	/* ml_io_map() does not return 0 when the io submap cannot serve a request: it
	 * reaches kmem_alloc(), which panic()s outright ("kmem_alloc(...): failed with
	 * 3 @vm_kern.c:180"). v157 and v158 both died there on the 16 MiB icache before
	 * any fallback could run, so the request is never made at all. Only sizes this
	 * board has already mapped are handed to it (the GPU windows above are up to
	 * 0x68000); everything larger uses the payload's own kernel mapping, which
	 * ShengGmu cleans before the CM3 is released. */
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

	ShengGmuServices services{};
	services.now_us = NativeAdrenoKernelTime::now_us;
	services.wait_us = NativeAdrenoKernelTime::wait_us;
	services.sha256 = gmu_sha256;
	services.flush = cacheable ? gmu_flush : nullptr;

	hummer_gmu_device = ShengGmuDevice{};
	hummer_gmu_device.io = io;
	hummer_gmu_device.services = services;

	boot_progress_stage("adreno-gmu: load fw");
	ShengGmuResult result{};
	const ShengGmuStatus status = ShengGmuLoadAndStartHfi(&hummer_gmu_device, &config, &result);

	if (hu_cb_offset) {
		hu_cb_sctlr = HU_RD(smmu, hu_cb_offset);
		hu_cb_fsr = HU_RD(smmu, hu_cb_offset + HU_CB_FSR);
		hu_cb_far = HU_RD(smmu, hu_cb_offset + HU_CB_FAR_LO);
		hu_cb_syn = HU_RD(smmu, hu_cb_offset + HU_CB_FSYNR0);
	}

	panic("hummer gmu: status=%u phase=%u fw=%08x hfi=%08x fwrdy=%u hfirdy=%u "
	      "hash=%u reset=%u\n"
	      "poll=%u ao=%08x cm3=%08x dtcm=%08x aod=%u uc=%u cc=%u\n"
	      "cb fsr=%08x far=%08x syn=%08x sctlr=%08x off=%x\n"
	      "clk cxo=%08x/%08x gmu=%08x/%08x ahb=%08x noc=%08x\n"
	      "ctx bank=%u asid=%u slot=%u sid=%u spans=%u ttbr0=%llx\n"
	      "iou: %llx %llx %llx %llx %llx",
	    status, result.phase, result.firmware_init_result, result.hfi_status,
	    result.firmware_ready_observed, result.hfi_ready_observed,
	    result.complete_hash_verified, result.reset_verified,
	    result.poll_reads, result.ao_fault_status, result.cm3_fault_status,
	    result.dtcm_version, result.ao_delta, uncached, cacheable,
	    hu_cb_fsr, hu_cb_far, hu_cb_syn, hu_cb_sctlr, hu_cb_offset,
	    hu_cxo_before, hu_cxo_after, hu_gmuclk_before, hu_gmuclk_after,
	    hu_ahb_after, hu_memnoc_after,
	    bank, asid, slot, sid, report.spans,
	    (unsigned long long)report.ttbr0,
	    (unsigned long long)payloads[0].iova, (unsigned long long)payloads[1].iova,
	    (unsigned long long)payloads[2].iova, (unsigned long long)payloads[3].iova,
	    (unsigned long long)payloads[4].iova);
}

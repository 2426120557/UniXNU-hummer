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

#define HU_SMMU_BASE  0x3da0000ULL
#define HU_SMMU_BYTES 0x40000ULL

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

extern "C" void hummer_adreno_bringup_probe(void)
{
	uint32_t enabled = 0;
	if (!PE_parse_boot_argn("hummer-adreno-bringup", &enabled, sizeof(enabled)) || !enabled)
		return;

	uint32_t cx_before = 0, cx_after = 0;
	boot_progress_stage("adreno-gmu: cx");
	if (!cx_power_up(&cx_before, &cx_after))
		panic("hummer gmu: CX did not come up gdscr=%08x -> %08x", cx_before, cx_after);

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

	uint32_t bank = 2, asid = 1, slot = 2, sid = SHENG_GPU_SMMU_GMU_SID;
	(void)PE_parse_boot_argn("hummer-adreno-bank", &bank, sizeof(bank));
	(void)PE_parse_boot_argn("hummer-adreno-asid", &asid, sizeof(asid));
	(void)PE_parse_boot_argn("hummer-adreno-slot", &slot, sizeof(slot));
	(void)PE_parse_boot_argn("hummer-adreno-sid", &sid, sizeof(sid));

	boot_progress_stage("adreno-gmu: smmu ctx");
	static NativeAdrenoGmuContextReport report;
	const IOReturn context_status = NativeAdrenoGmuContextBringup(
	    reinterpret_cast<volatile uint8_t *>(smmu), HU_SMMU_BYTES,
	    bank, asid, slot, sid, ctx_progress, &report);

	static NativeAdrenoGmuPayload payloads[5];
	const IOReturn payload_status = NativeAdrenoGmuContextPayloads(payloads);
	if (context_status || payload_status)
		panic("hummer gmu: context failed status=%x step=%u inner=%x payload=%x "
		      "id1=%08x id2=%08x ttbr0=%llx spans=%u sctlr=%08x smr=%08x s2cr=%08x",
		    context_status, report.step, report.status, payload_status,
		    report.id1, report.id2, (unsigned long long)report.ttbr0, report.spans,
		    report.sctlr, report.smr, report.s2cr);

	ShengGmuConfig config{};
	config.chip_id = SHENG_GMU_CHIP_ID;
	config.firmware = native_adreno_firmware_image;
	config.firmware_bytes = static_cast<size_t>(native_adreno_firmware_image_size);
	config.poll_budget = 1000;
	ShengGmuMemory *memory[5] = { &config.icache, &config.dummy, &config.debug,
	                              &config.log, &config.hfi };
	for (unsigned i = 0; i < 5; ++i) {
		vm_offset_t view = ml_io_map(payloads[i].physical, static_cast<vm_size_t>(payloads[i].bytes));
		if (!view)
			panic("hummer gmu: payload %u unmappable pa=%llx bytes=%llx", i,
			    (unsigned long long)payloads[i].physical,
			    (unsigned long long)payloads[i].bytes);
		memory[i]->cpu = reinterpret_cast<volatile uint32_t *>(view);
		memory[i]->bytes = static_cast<size_t>(payloads[i].bytes);
		memory[i]->iova = payloads[i].iova;
	}

	ShengGmuServices services{};
	services.now_us = NativeAdrenoKernelTime::now_us;
	services.wait_us = NativeAdrenoKernelTime::wait_us;
	services.sha256 = gmu_sha256;

	hummer_gmu_device = ShengGmuDevice{};
	hummer_gmu_device.io = io;
	hummer_gmu_device.services = services;

	boot_progress_stage("adreno-gmu: load fw");
	ShengGmuResult result{};
	const ShengGmuStatus status = ShengGmuLoadAndStartHfi(&hummer_gmu_device, &config, &result);

	panic("hummer gmu: status=%u phase=%u fw_result=%08x hfi=%08x "
	      "fw_ready=%u hfi_ready=%u hash=%u reset=%u ao=%08x cm3=%08x "
	      "poll=%u w=%u r=%u\n"
	      "ctx bank=%u asid=%u slot=%u sid=%u ttbr0=%llx spans=%u\n"
	      "iou: %llx %llx %llx %llx %llx",
	    status, result.phase, result.firmware_init_result, result.hfi_status,
	    result.firmware_ready_observed, result.hfi_ready_observed,
	    result.complete_hash_verified, result.reset_verified,
	    result.ao_fault_status, result.cm3_fault_status,
	    result.poll_reads, result.write_attempts, result.read_attempts,
	    bank, asid, slot, sid, (unsigned long long)report.ttbr0, report.spans,
	    (unsigned long long)payloads[0].iova, (unsigned long long)payloads[1].iova,
	    (unsigned long long)payloads[2].iova, (unsigned long long)payloads[3].iova,
	    (unsigned long long)payloads[4].iova);
}

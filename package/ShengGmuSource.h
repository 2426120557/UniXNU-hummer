/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * SM8750 (hummer, Adreno 830 / gen8_0_0) GMU register and window inputs.
 *
 * Every value below is derived, not guessed.  Two sources:
 *
 *  1. Register offsets: the dword offsets pinned in
 *     drivers/gpu/drm/msm/registers/adreno/a6xx.xml and a6xx_gmu.xml at
 *     sm8550-mainline 42f3b40c702abbc37c3a04c95af008cd283aeaf3, selecting the
 *     A8XX variants ("A8XX-", or no variants attribute).  The same files back
 *     scripts/derive-a740-hwinit.py for A7XX, so the two generations are
 *     compared from one source.
 *     The GMU register block did NOT move between A7XX and A8XX; the CP, RBBM
 *     and UCHE blocks did.  The three registers that are generation-specific
 *     here are GMU_GENERAL_8/9/10 (A7XX 0x1f9cd/0x1f9ce/0x1f9cf) and
 *     GPU_GMU_CX_GMU_CX_FAL_INTF / FALNEXT_INTF (A6XX 0x1f8f0/0x1f8f1).
 *
 *  2. Windows and the GMU-to-GPU aperture delta: the hummer device tree
 *     (Resources/DTBs/hummer.dts in the boot UEFI tree), node
 *     qcom,gmu@3d37000 with reg-names "gmu", "gmu_ao_blk_dec0" and node
 *     qcom,kgsl-3d0@3d00000 with reg-names "kgsl_3d0_reg_memory", "rscc",
 *     "cx_dbgc", "cx_misc", ...:
 *       gmu  0x03d37000 + 0x68000     kgsl 0x03d00000 + 0x40000
 *       rscc 0x03d50000 + 0x10000     cx_misc 0x03d9e000 + 0x2000
 *     so GMU word n lives at gpu_base + 4*n - SHENG_GMU_CORE_GPU_DELTA with
 *     SHENG_GMU_CORE_GPU_DELTA = 0x3d37000 - 0x3d00000 = 0x37000.  SM8550's
 *     delta is 0x6a000, which is why that constant cannot be inherited.
 *
 * Differences from the SM8550 board this kit is modelled on, each traced to
 * a6xx_gmu.c:
 *   - There is no "gmu_pdc" region: the GPU node has no PDC resource, and
 *     a6xx_gmu_rpmh_init() returns immediately for A8XX because "RPMH/PDC
 *     configurations are entirely configured in AOP".  The RSC/PDC setup table
 *     is therefore empty and the PDC window must be absent.
 *   - a6xx_gmu.c skips CX_MISC_TCM_RET_CNTL for A8XX (A7XX writes it), so this
 *     kit never touches the cx_misc window.
 *   - The AHB fence window is 0x8c0..0x32 on A8XX, not 0x8a0..0x32.
 *   - The CX GBIF programming comes from the pinned a840_gbif[] list, which is
 *     the A8XX list: QSB_SIDE0..3 = 0x71e20 (not a610's 0x71620) plus
 *     GBIF_CX_CONFIG = 0x20023000, and the GMU MRC QOS word 0x33.
 */
#ifndef SHENG_GMU_SOURCE_H
#define SHENG_GMU_SOURCE_H
#include <stdint.h>
#include <stddef.h>

/* --- GMU core registers (a6xx_gmu.xml, A8XX) --------------------------- */
#define REG_GMU_CM3_ITCM_START          UINT32_C(0x1b400)
#define REG_GMU_CM3_DTCM_START          UINT32_C(0x1c400)
#define REG_GMU_CM3_SYSRESET            UINT32_C(0x1f800)
#define REG_GMU_CM3_BOOT_CONFIG         UINT32_C(0x1f801)
#define REG_GMU_CM3_FW_INIT_RESULT      UINT32_C(0x1f81c)
#define REG_GMU_CM3_CFG                 UINT32_C(0x1f82d)
#define REG_GMU_HFI_QTBL_INFO           UINT32_C(0x1f984)
#define REG_GMU_HFI_QTBL_ADDR           UINT32_C(0x1f985)
#define REG_GMU_HFI_CTRL_INIT           UINT32_C(0x1f986)
#define REG_GMU_HFI_CTRL_STATUS         UINT32_C(0x1f980)
#define REG_GMU_HOST2GMU_INTR_SET       UINT32_C(0x1f994)
#define REG_GMU_GMU2HOST_INTR_INFO      UINT32_C(0x1f992)
#define REG_GMU_SYS_BUS_CONFIG          UINT32_C(0x1f40f)
#define REG_GMU_ICACHE_CONFIG           UINT32_C(0x1f400)
#define REG_GMU_DCACHE_CONFIG           UINT32_C(0x1f401)
#define REG_GMU_AHB_FENCE_RANGE_0       UINT32_C(0x23b11)
#define REG_GMU_AO_HOST_INTERRUPT_STATUS UINT32_C(0x23b05)
#define REG_GMU_MRC_GBIF_QOS_CTRL       UINT32_C(0x1f50b)
/* gen8_gmu_register_config(): the GMU's own power state and its two FAL
 * interfaces, all three written before the CM3 is released. Absent from the
 * SM8550 kit's A7XX order; the annibale port (SM8750, same register file) does
 * all three and its GMU started with them. */
#define REG_GMU_RPMH_POWER_STATE        UINT32_C(0x1f7e9)
/* Generation-specific (A8XX variants). */
#define REG_GMU_GENERAL_8               UINT32_C(0x1f9c8)
#define REG_GMU_GENERAL_9               UINT32_C(0x1f9c9)
#define REG_GMU_GENERAL_10              UINT32_C(0x1f9ca)
#define REG_GMU_CX_FAL_INTF             UINT32_C(0x1f7ec)
#define REG_GMU_CX_FALNEXT_INTF         UINT32_C(0x1f7ed)
/* The GMU's crystal counter: it has to tick for the firmware to have run at
 * all, so the loader prints how far it moved while the CM3 was out of reset. */
#define REG_GMU_ALWAYS_ON_LO            UINT32_C(0x1f840)

/* --- GPU window registers used before the GMU is running (a6xx.xml) ----- */
#define REG_GBIF_QSB_SIDE0              UINT32_C(0x3c03)
#define REG_GBIF_CX_CONFIG              UINT32_C(0x3c00)
/* GEN8_GBIF_HALT, dword 0x3c45: bit 3 releases the GMU's bus interface. The
 * annibale port clears it as part of gen8_gmu_register_config (right after the
 * four QSB sides, CX config and the MRC QoS word); without it the GMU has no
 * path to the memory its firmware lives in. */
#define REG_GPU_GBIF_HALT               UINT32_C(0x3c45)
/* --- RSCC window registers (a6xx_gmu.xml; no variants, so shared) ------- */
/* Word 0x0101 -> byte 0x404 inside the rscc aperture.  The loader reads it to
 * refuse a board whose RSC sequencer is still busy. */
#define REG_RSCC_SEQ_BUSY_DRV0          UINT32_C(0x0101)

/* --- Identity ---------------------------------------------------------- */
/* Real silicon id, read by the v142 probe: "hummer: adreno probe done
 * (Adreno830v2 chipid 0x44050001)".  Linux compares the configured chip id
 * against the platform's discovery; nothing is guessed here. */
#define SHENG_GMU_CHIP_ID               UINT32_C(0x44050001)
/* The word written to GMU_GENERAL_10, which the firmware reads back to learn
 * which chip it runs on.  0x08000000 was INFERRED from the catalog's pattern
 * (gen80300 -> gmu_chipid 0x8030000), i.e. gen80000 -> patch level 0.
 * v160 replaces it with the one actually observed on this silicon's twin: the
 * annibale port (SM8750, live DT gen8_0_1, chip ID 0x44050001 -- the very id
 * the v142 probe read from this board) writes 0x08000100 there, which is
 * major 8 / minor 0 / patch 1 in the vendor's ADRENO_GMU_REV encoding
 * (bits 31:8).  hummer never got its CM3 to signal FW_INIT_RESULT with
 * 0x08000000, so the patch level now matches the part. */
#define SHENG_GMU_FW_CHIP_ID            UINT32_C(0x08000100)

/* --- Aperture geometry -------------------------------------------------- */
#define SHENG_GMU_CORE_GPU_DELTA        UINT32_C(0x37000)
/* A8XX AHB fence window: a6xx_gmu.c uses upper 0x32 / lower 0x8c0 for a8xx
 * (0x8a0 for a7xx, 0xa0 for a6xx). */
#define SHENG_GMU_FENCE_UPPER           UINT32_C(0x32)
#define SHENG_GMU_FENCE_LOWER           UINT32_C(0x8c0)
/* a840_gbif[] from the pinned catalog, the only A8XX GBIF list published. */
#define SHENG_GMU_GBIF_QSB_SIDE_VALUE   UINT32_C(0x71e20)
#define SHENG_GMU_GBIF_CX_CONFIG_VALUE  UINT32_C(0x20023000)
#define SHENG_GMU_MRC_QOS_VALUE         UINT32_C(0x33)
/* GEN8_GBIF_HALT bit 3. */
#define SHENG_GMU_GBIF_HALT_RELEASE     UINT32_C(0x8)

/* --- Fatal conditions (bitfields of the two status registers) ----------- */
/* GMU_AO_HOST_INTERRUPT_STATUS: WDOG_BITE 0, FENCE_ERR 3, HOST_AHB_BUS_ERROR 5. */
#define SHENG_GMU_AO_FATAL_MASK         UINT32_C(0x29)
/* GMU_GMU2HOST_INTR_INFO: CM3_FAULT 23. */
#define SHENG_GMU_CM3_FATAL_MASK        UINT32_C(0x800000)

/* --- Firmware policy --------------------------------------------------- */
/* qcom/gen80000_gmu.bin from linux-firmware, 88896 bytes,
 * sha256 1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5.
 * Only the complete image hash is accepted; AdrenoFirmwarePlanGmu() then
 * splits it into the 15 records the loader expects.  The blob never enters
 * Git: scripts/fetch-hummer-gpu-firmware.py downloads and verifies it. */
#define SHENG_GMU_FIRMWARE_BYTES        88896u
static const uint8_t sheng_gmu_firmware_sha256[32]={
    0x1a,0xd8,0x17,0x5e,0x6c,0xd0,0x1e,0xa7,0x6c,0x64,0xd2,0x0b,0x83,0x60,0x00,0x38,
    0xc5,0x2d,0xd2,0x98,0x44,0xb4,0x8d,0x57,0xb1,0x1b,0x76,0x9b,0x03,0x88,0x63,0xa5,
};

/* --- Windows, in ShengGmuCore..ShengGmuCxMisc order -------------------- */
/* PDC is absent on SM8750 (see the header comment): physical 0, size 0. */
static const uint64_t sheng_gmu_window_physical[5]={
    0x03d37000ull, /* Core   gmu@3d37000                      */
    0x03d50000ull, /* Rscc   reg-names "rscc"                */
    0x00000000ull, /* Pdc    absent on SM8750                */
    0x03d00000ull, /* Gpu    kgsl_3d0_reg_memory@3d00000     */
    0x03d9e000ull, /* CxMisc reg-names "cx_misc"             */
};
static const size_t sheng_gmu_window_bytes[5]={
    0x68000, /* gmu */
    0x10000, /* rscc */
    0x0,     /* gmu_pdc: no such resource in the device tree */
    0x40000, /* kgsl_3d0_reg_memory */
    0x2000,  /* cx_misc */
};
/* Whether the PDC window must be present.  A8XX programs no PDC (AOP owns it),
 * so a board that has one still does not use it through this kit. */
#define SHENG_GMU_PDC_PRESENT 0

/* --- RSC/PDC setup writes ---------------------------------------------- */
/* Empty on A8XX: a6xx_gmu_rpmh_init() returns before any RSC or PDC write
 * because AOP owns that configuration.  Kept as a table so the loader code and
 * the SM8550 kit stay structurally identical. */
typedef struct {unsigned window;size_t byte_offset;uint32_t value;} ShengGmuSetupWrite;
#define SHENG_GMU_RSC_SETUP_COUNT 0u
static const ShengGmuSetupWrite sheng_gmu_rsc_setup[1]={{0,0,0}};
#endif

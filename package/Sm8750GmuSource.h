/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
// Linux 65ba8f34ec886ec72b2c07ddc8cf3d815c32869d: a6xx_gmu.xml, a6xx.xml, a8xx_catalog.c.
// MMIO: PKX110 runtime DTB. Firmware pin: device scripts/data/sm8750-gpu-inputs.json.
#define SM8750_GMU_CORE_GPU_DELTA 0x37000u
#define SM8750_GMU_CHIP_ID UINT32_C(0x44050001)
#define SM8750_GMU_FW_CHIP_ID UINT32_C(0x08000100)
#define SM8750_GMU_FIRMWARE_BYTES 88896u
#define SM8750_GMU_AO_FATAL_MASK 0x29u
#define SM8750_GMU_CM3_FATAL_MASK (1u << 23)
#define REG_GBIF_CX_CONFIG 0x3c00u
#define REG_GBIF_QSB_SIDE0 0x3c03u
#define REG_GMU_AHB_FENCE_RANGE_0 0x23b11u
#define REG_GMU_AO_HOST_INTERRUPT_STATUS 0x23b05u
#define REG_GMU_CM3_BOOT_CONFIG 0x1f801u
#define REG_GMU_CM3_CFG 0x1f82du
#define REG_GMU_CM3_DTCM_START 0x1c400u
#define REG_GMU_CM3_FW_INIT_RESULT 0x1f81cu
#define REG_GMU_CM3_ITCM_START 0x1b400u
#define REG_GMU_CM3_SYSRESET 0x1f800u
#define REG_GMU_DCACHE_CONFIG 0x1f401u
#define REG_GMU_GENERAL_10 0x1f9cau
#define REG_GMU_GENERAL_8 0x1f9c8u
#define REG_GMU_GENERAL_9 0x1f9c9u
#define REG_GMU_GMU2HOST_INTR_INFO 0x1f992u
#define REG_GMU_GMU2HOST_INTR_CLR 0x1f991u
#define REG_GMU_HFI_CTRL_INIT 0x1f986u
#define REG_GMU_HFI_CTRL_STATUS 0x1f980u
#define REG_GMU_HFI_QTBL_ADDR 0x1f985u
#define REG_GMU_HFI_QTBL_INFO 0x1f984u
#define REG_GMU_HOST2GMU_INTR_SET 0x1f994u
#define REG_GMU_ICACHE_CONFIG 0x1f400u
#define REG_GMU_MRC_GBIF_QOS_CTRL 0x1f50bu
#define REG_GMU_SYS_BUS_CONFIG 0x1f40fu
#define REG_GPU_GMU_CX_GMU_CX_FALNEXT_INTF 0x1f7edu
#define REG_GPU_GMU_CX_GMU_CX_FAL_INTF 0x1f7ecu
#define REG_RSCC_SEQ_BUSY_DRV0 0x0101u
static const uint64_t sm8750_gmu_window_physical[]={0x3d37000,0x3d50000,0x3d00000};
static const size_t sm8750_gmu_window_bytes[]={0x68000,0x10000,0x40000};
static const uint8_t sm8750_gmu_firmware_sha256[32]={0x1a,0xd8,0x17,0x5e,0x6c,0xd0,0x1e,0xa7,0x6c,0x64,0xd2,0x0b,0x83,0x60,0x00,0x38,0xc5,0x2d,0xd2,0x98,0x44,0xb4,0x8d,0x57,0xb1,0x1b,0x76,0x9b,0x03,0x88,0x63,0xa5};

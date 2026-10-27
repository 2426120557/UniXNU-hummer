/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * SM8750 (hummer) GPU SMMU identity and stream routing.
 *
 * Source: the hummer device tree (Resources/DTBs/hummer.dts in the boot UEFI
 * tree), node qcom,kgsl-iommu@3da0000 reg = <0x3da0000 0x40000> and its three
 * context-bank children:
 *
 *   gfx3d_user    iommus = <0x275 0x00 0x00>   stream id 0
 *   gfx3d_lpac    iommus = <0x275 0x01 0x00>   stream id 1
 *   gfx3d_secure  iommus = <0x275 0x02 0x00>   stream id 2 (secure, not routed
 *                                              by this kit)
 *
 * and node qcom,gmu@3d37000 iommus = <0x275 0x05 0x00>, so the GMU's stream is
 * id 5.  The mask cell is 0 in every entry, i.e. each line names exactly one
 * stream id.
 *
 * The GPU owns bank 0 with the two non-secure streams (BR/rendering and LPAC);
 * the GMU owns a nonzero bank with the single stream 5.  Which bank the GMU
 * gets is a driver argument (NativeAdrenoGmuContext), not a DT property, so it
 * stays a caller input here.
 *
 * The same device tree records qcom,chipid = <0x44050001> and
 * qcom,gpu-model = "Adreno830v2" on qcom,kgsl-3d0@3d00000, which matches the
 * value the v142 register probe printed, so ShengGmuSource.h and this file
 * describe the same silicon.
 */
#ifndef SHENG_GPU_SMMU_SOURCE_H
#define SHENG_GPU_SMMU_SOURCE_H
#include <stdint.h>
#include <stddef.h>

#define SHENG_GPU_SMMU_BASE  0x03da0000ull
#define SHENG_GPU_SMMU_BYTES 0x40000u

/* Streams the GPU context routes: user (0) and LPAC (1). */
#define SHENG_GPU_SMMU_GPU_STREAMS 2u
static const unsigned sheng_gpu_smmu_sids[SHENG_GPU_SMMU_GPU_STREAMS]={0u,1u};

/* The GMU's own stream. */
#define SHENG_GPU_SMMU_GMU_SID 5u
#endif

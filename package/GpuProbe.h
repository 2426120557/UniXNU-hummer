// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// Register facts: OnePlusOSS/android_kernel_oneplus_sm8750 at
// 745702d5ed2c0435f2b1d3bb78bc4f48422d4191, drivers/clk/qcom/gpucc-sun.c.
#define SM8750_GPUCC_BASE 0x03d90000u
#define SM8750_GPU_SMMU_BASE 0x03da0000u

enum sm8750_gpu_cc_index {
    GPU_CX_POWER, GPU_CX_RESET, GPU_AHB_CLOCK, GPU_SMMU_CLOCK,
    GPU_CX_HW_CONTROL, GPU_CX_GMU_CLOCK, GPU_XO_CLOCK,
    GPU_HUB_CLOCK, GPU_HUB_CX_CLOCK, GPU_GMU_CONFIG, GPU_HUB_CONFIG,
    GPU_CC_COUNT
};

struct sm8750_gpu_register { const char *name; uint32_t offset; };
static const struct sm8750_gpu_register sm8750_gpu_cc_registers[] = {
    {"CX_GDSCR", 0x9080}, {"CX_BCR", 0x907c},
    {"AHB_CBCR", 0x90bc}, {"SMMU_CBCR", 0x7000},
    {"CX_HW_CTRL", 0x9094}, {"CX_GMU_CBCR", 0x90d4},
    {"XO_CBCR", 0x90e4}, {"HUB_AON_CBCR", 0x93e8},
    {"HUB_CX_CBCR", 0x90e8}, {"GMU_CFG", 0x931c}, {"HUB_CFG", 0x93f0}
};
static const struct sm8750_gpu_register sm8750_gpu_smmu_registers[] = {
    {"SMMU_IDR0", 0x20}, {"SMMU_IDR1", 0x24}, {"SMMU_IDR2", 0x28},
    {"SMMU_sCR0", 0}, {"SMMU_GFSR", 0x48}
};
#define GPU_SMMU_COUNT 5

enum sm8750_gpu_probe_result {
    GPU_PROBE_OK, GPU_PROBE_SMMU_SKIPPED, GPU_PROBE_READ_FAILED,
    GPU_PROBE_INVALID
};
struct sm8750_gpu_snapshot {
    uint32_t cc[GPU_CC_COUNT], smmu[GPU_SMMU_COUNT];
    unsigned cc_count, smmu_count;
};

// A retained clock without our vote can disappear. Require an enabled, running
// branch, CX power-on and reset deasserted; never acquire power for this probe.
static inline int sm8750_gpu_smmu_readable(const struct sm8750_gpu_snapshot *s)
{
    return s->cc_count == GPU_CC_COUNT &&
        s->cc[GPU_CX_POWER] != UINT32_MAX &&
        (s->cc[GPU_CX_POWER] & 0x80000001u) == 0x80000000u &&
        s->cc[GPU_CX_RESET] != UINT32_MAX && !(s->cc[GPU_CX_RESET] & 1) &&
        (s->cc[GPU_AHB_CLOCK] & 0x80000001u) == 1 &&
        (s->cc[GPU_SMMU_CLOCK] & 0x80000001u) == 1;
}

// The transport exposes reads only. It must report the address before touching
// MMIO so a bus fault still leaves a useful last checkpoint on the display.
static inline enum sm8750_gpu_probe_result sm8750_gpu_probe(
    int (*read32)(void *, const char *, uint32_t, uint32_t *), void *context,
    struct sm8750_gpu_snapshot *s)
{
    if (!read32 || !s) return GPU_PROBE_INVALID;
    *s = (struct sm8750_gpu_snapshot){0};
    for (unsigned i = 0; i < GPU_CC_COUNT; ++i) {
        const struct sm8750_gpu_register *r = &sm8750_gpu_cc_registers[i];
        if (read32(context, r->name, SM8750_GPUCC_BASE + r->offset, &s->cc[i]))
            return GPU_PROBE_READ_FAILED;
        ++s->cc_count;
    }
    if (!sm8750_gpu_smmu_readable(s)) return GPU_PROBE_SMMU_SKIPPED;
    for (unsigned i = 0; i < GPU_SMMU_COUNT; ++i) {
        const struct sm8750_gpu_register *r = &sm8750_gpu_smmu_registers[i];
        if (read32(context, r->name, SM8750_GPU_SMMU_BASE + r->offset, &s->smmu[i]))
            return GPU_PROBE_READ_FAILED;
        ++s->smmu_count;
        if (i == 0 && (!s->smmu[0] || s->smmu[0] == UINT32_MAX))
            return GPU_PROBE_INVALID;
    }
    return GPU_PROBE_OK;
}

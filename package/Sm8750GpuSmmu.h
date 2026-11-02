/* SPDX-License-Identifier: MIT */
#ifndef SM8750_GPU_SMMU_H
#define SM8750_GPU_SMMU_H
#include "ArmSmmu4KProgram.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t id0,id1,offset,value;
    unsigned phase,banks[2],slots[3]; /* GPU, GMU banks; GPU SID 0/1, GMU SID 5 slots. */
    size_t bank_offset[2];
} Sm8750GpuSmmuResult;
/* Masters must remain reset. Preserve live routes; only fault unused slots. */
int Sm8750GpuSmmuPrepare(const ArmSmmu4KIo *,Sm8750GpuSmmuResult *);
#ifdef __cplusplus
}
#endif
#endif

/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "Rpmh.h"
#include "Sm8750GmuCommandDb.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {Sm8750PowerGcc,Sm8750PowerGpucc};
typedef struct {
    void *cookie;
    int (*read)(void *,unsigned window,uint32_t offset,uint32_t *);
    int (*write)(void *,unsigned window,uint32_t offset,uint32_t);
    int (*delay)(void *,unsigned microseconds);
    /* Acquire shared CX/MX votes at or above these corner levels. The owner
     * must aggregate with display/other users and keep both votes on failure. */
    int (*rails)(void *,uint32_t cx_level,uint32_t mx_level);
} Sm8750GpuPowerIo;
typedef struct {
    uint32_t attempted,ready,stage,window,offset,value,reads,writes,retain_resources;
} Sm8750GpuPowerResult;
/* Cold GMU 500 MHz / hub 200 MHz with PLL0 at 1 GHz. The caller owns GPUCC,
 * serializes GCC updates and retains the state/mappings after any mutation.
 * A running PLL at another rate, asserted reset or active hardware owner is
 * refused. This does not power GX, start firmware or touch RSC/PDC firmware. */
int Sm8750GpuPowerOn(const Sm8750GpuPowerIo *,Sm8750GpuPowerResult *);
/* Build ARC requests from a complete command-db. cx/mx are aggregated corner
 * levels from the owner, not millivolts or raw indices. Output is transactional. */
int Sm8750GpuRailCommands(const Sm8750GmuVoteInputs *,uint32_t cx,uint32_t mx,struct rpmh_command out[2]);
#ifdef __cplusplus
}
#endif

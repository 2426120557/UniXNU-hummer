/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *cookie;
    int (*invoke)(void *,uint64_t registers[8]);
    int (*now_us)(void *,uint64_t *);
    int (*wait_us)(void *,unsigned);
    int (*publish)(void *,const void *,size_t);
} Sm8750ScmIo;
typedef struct {int64_t transport;uint64_t result;unsigned calls;} Sm8750ScmResult;
int Sm8750ScmCall(const Sm8750ScmIo *,uint32_t function,uint32_t arginfo,
    uint64_t arg0,uint64_t arg1,uint64_t arg2,Sm8750ScmResult *);
int Sm8750ScmInvoke(void *,uint64_t registers[8]);
/* Required secure CX initialization before CM3 start: ALWAYS_EN + TSENSE. */
int Sm8750GpuSecureCxInit(const Sm8750ScmIo *,Sm8750ScmResult *);
typedef struct {unsigned phase,authenticated;Sm8750ScmResult scm;} Sm8750GpuZapResult;
/* Inputs are produced from the pinned signed MDT by prepare-gpu-firmware.py.
 * Caller owns a coherent 16 KiB metadata allocation and the reserved
 * 0x9b09a000/0x2000 mapping. Retain both after any SMC; no retry or cleanup
 * of an uncertain PAS state. No direct SECVID_TRUST_CNTL write is used. */
int Sm8750GpuZapLoad(const Sm8750ScmIo *,const uint8_t *metadata,size_t metadata_bytes,
    const uint8_t *payload,size_t payload_bytes,void *metadata_cpu,uint64_t metadata_pa,
    volatile uint8_t *reservation,Sm8750GpuZapResult *);
#ifdef __cplusplus
}
#endif

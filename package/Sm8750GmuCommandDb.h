/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "AdrenoHfiVotes.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint32_t address;AdrenoHfiRail aux;} Sm8750GmuBcmInput;
typedef struct {
    AdrenoHfiRail gfx,mx,cx,gmxc,gbx;
    Sm8750GmuBcmInput sh0,mc0,acv;
    uint32_t cn0_address;
    uint32_t cx_address,mx_address;
} Sm8750GmuVoteInputs;
typedef enum {Sm8750GmuDbSuccess,Sm8750GmuDbArgument,Sm8750GmuDbVersion,
    Sm8750GmuDbCorrupt,Sm8750GmuDbDuplicate,Sm8750GmuDbMissing} Sm8750GmuDbStatus;
/* Complete immutable snapshot of 0x81c60000/0x20000 from the platform.
 * Validates format version 1 and offsets, not authenticity. Returned spans
 * alias the snapshot and must outlive startup. Output stays unchanged on error. */
Sm8750GmuDbStatus Sm8750GmuCommandDbInputs(const uint8_t *,size_t,Sm8750GmuVoteInputs *);
#ifdef __cplusplus
}
#endif

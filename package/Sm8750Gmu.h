/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM8750_GMU_H
#define SM8750_GMU_H
#include "AdrenoHfiStartup.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {Sm8750GmuCore,Sm8750GmuRscc,Sm8750GmuGpu,Sm8750GmuWindows};
typedef enum {
    Sm8750GmuSuccess,Sm8750GmuArgument,Sm8750GmuFirmware,Sm8750GmuBusy,
    Sm8750GmuIoError,Sm8750GmuStateMismatch,Sm8750GmuTimeout,Sm8750GmuClockError,
    Sm8750GmuPoisoned,Sm8750GmuFault,Sm8750GmuProtocolError
} Sm8750GmuStatus;
typedef struct {
    void *cookie;
    size_t bytes[Sm8750GmuWindows];
    int (*read32)(void *,unsigned window,size_t byte_offset,uint32_t *);
    int (*write32)(void *,unsigned window,size_t byte_offset,uint32_t);
    int (*barrier)(void *);
} Sm8750GmuIo;
typedef struct {
    void *cookie;
    int (*sha256)(void *,const void *,size_t,uint8_t digest[32]);
    int (*now_us)(void *,uint64_t *);
    int (*wait_us)(void *,unsigned);
} Sm8750GmuServices;
typedef struct {volatile uint32_t *cpu;size_t bytes;uint64_t iova;} Sm8750GmuMemory;
typedef struct {
    uint32_t chip_id;
    const uint8_t *firmware;size_t firmware_bytes;
    Sm8750GmuMemory icache,dummy,debug,log,hfi;
    unsigned poll_budget;
} Sm8750GmuConfig;
typedef struct {
    Sm8750GmuIo io;Sm8750GmuServices services;AdrenoHfi hfi;
    uint32_t poisoned,boot_attempted,gpu_oob_held,gpu_levels,bandwidth_levels;
    AdrenoHfiStartup startup;
} Sm8750GmuDevice;
typedef struct {
    uint32_t phase,write_attempts,read_attempts,poll_reads;
    uint32_t complete_hash_verified,memory_changed,reset_verified;
    uint32_t firmware_init_result,hfi_status,firmware_ready_observed,hfi_ready_observed;
    uint32_t ao_fault_status,cm3_fault_status;
    uint32_t resources_must_retain;
    uint8_t complete_sha256[32];
} Sm8750GmuResult;

/* Cold firmware/HFI startup for A830v2. Caller owns power, clocks, RPMh votes,
 * native-4K GMU mappings and coherent memory, with CM3 held in reset and RSC
 * idle. AOP owns RSC/PDC configuration on SM8750; this code does not replace it.
 * Validates the pinned firmware, writes/reads back TCM and polls fresh status.
 * Hash/time/wait callbacks must perform real, bounded operations. Failures after
 * mutation poison the device and attempt reset, retaining every allocation.
 * Success covers FW/HFI observations, not CP, rendering or service publication. */
Sm8750GmuStatus Sm8750GmuLoadAndStartHfi(Sm8750GmuDevice *,const Sm8750GmuConfig *,Sm8750GmuResult *);

typedef struct {
    unsigned polls_per_command,wait_interval_us;
    uint64_t command_timeout_us,total_timeout_us;
} Sm8750GmuStartupLimits;
typedef struct {
    Sm8750GmuResult load;
    AdrenoHfiStartupResult protocol;
    AdrenoHfiStartupStatus protocol_status;
    Sm8750GmuStatus health_status;
} Sm8750GmuStartupResult;
/* Full cold firmware/HFI startup sequence, under all LoadAndStartHfi platform
 * preconditions above. Validate protocol/limits before any hardware mutation,
 * then load firmware and exchange every required command with actual ACKs.
 * Health checks reread reset/HFI status and fatal registers around commands.
 * Supplied OPP/ARC/BCM/ACD data must be platform-derived; this function neither
 * invents command-db values nor sets power, clocks, SMMU or AOSS policy.
 * Any protocol failure after the load attempts reset and permanently poisons
 * the device. A verified reset still does not authorize releasing mappings.
 * Success covers firmware startup exchanges, NOT SQE/ZAP/GPU initialization,
 * shader execution, submission, presentation or a complete hardware driver. */
Sm8750GmuStatus Sm8750GmuColdStart(Sm8750GmuDevice *,const Sm8750GmuConfig *,
    const AdrenoHfiStartupConfig *,const Sm8750GmuStartupLimits *,Sm8750GmuStartupResult *);

/* Serialized runtime operations, after ColdStart. Hold OOB while touching GX
 * or submitting work. Votes use ascending HFI table indices, never DT indices.
 * A failure poisons the session but does not reset a possibly active GPU. */
Sm8750GmuStatus Sm8750GmuGpuOob(Sm8750GmuDevice *,unsigned acquire,Sm8750GmuResult *);
Sm8750GmuStatus Sm8750GmuSetPerformance(Sm8750GmuDevice *,unsigned frequency,unsigned bandwidth,Sm8750GmuResult *);

typedef struct {volatile unsigned char *cpu;uint64_t physical;size_t bytes;} Sm8750GmuMmioWindow;
typedef struct {Sm8750GmuMmioWindow windows[Sm8750GmuWindows];} Sm8750GmuMmio;
/* Bind only already owned/powered Device mappings of exact pinned DT ranges.
 * Uses direct AArch64 volatile 32-bit I/O + dsb sy. No map/allocation/power,
 * pointer ownership proof or external-abort recovery is provided. */
Sm8750GmuStatus Sm8750GmuBindMmio(Sm8750GmuMmio *,Sm8750GmuIo *);
#ifdef __cplusplus
}
#endif
#endif

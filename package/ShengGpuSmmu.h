/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_GPU_SMMU_H
#define SHENG_GPU_SMMU_H
#include "ArmSmmu4KStreams.h"
#include "ArmSmmu4KAudit.h"
#include "ArmSmmu4KLive.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {ShengGpuSmmuGpu,ShengGpuSmmuGmu} ShengGpuSmmuRole;
typedef struct {
    unsigned attempted,ready,poisoned;
    ShengGpuSmmuRole role;
    ArmSmmu4KContextConfig context;
    ArmSmmu4KAuditConfig audit;
    ArmSmmu4KStreamState streams;
    const ArmSmmu4KLiveArena *live_arena;
    ArmSmmu4KLiveState *live_state;
} ShengGpuSmmuSession;
typedef struct {
    unsigned phase,must_retain_resources;
    ArmSmmu4KProgramResult context;
    ArmSmmu4KStreamResult streams;
    ArmSmmu4KAuditResult audit;
    ArmSmmu4KLiveResult live;
    ArmSmmu4KStatus cleanup_status;
} ShengGpuSmmuResult;
/* Bind pinned board SIDs to real native4K context/streams. GPU owns bank 0 and
 * upper TTBR1 (optional lower TTBR0); GMU owns a nonzero bank and lower TTBR0.
 * Require exactly the pinned DT MMIO reservation and role-specific SID count.
 * Slot numbers are actual exclusive leases, never assumed SID==slot.
 * This does NOT acquire power/clocks/global ownership, map MMIO, perform MMU500
 * reset/ACTLR/SCM setup, allocate/cache-clean tables, install IRQs, establish
 * physical mappings or stop masters. Those must already be done by platform.
 * Masters stay quiesced until success; a single global configuration lease
 * spans preflight, context installation and stream publication. All config,
 * result, session and pointed-to table/config storage is disjoint and retained.
 * Context uses fault-continuation as required by the pinned Adreno SMMU writer.
 * Physical graph audit is mandatory before register writes and after streams
 * attach. Active audit uses observed TTBR roots, with context/stream checks
 * before and after. Actual physical mapping/coherence and device execution
 * still require platform qualification. No synthesized IDs or readbacks.
 * Failure permits context disable ONLY after all its streams are proved FAULT,
 * avoiding stage1 bypass through a disabled bank. Never automatically retry.
 * All outcomes retain resources; successful reset/TLB sync is not a bus drain. */
ArmSmmu4KStatus ShengGpuSmmuStart(const ArmSmmu4KIo *,uint64_t register_physical,
    ShengGpuSmmuRole,unsigned bank,const unsigned *slots,unsigned count,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KAuditConfig *,unsigned poll_budget,ShengGpuSmmuSession *,ShengGpuSmmuResult *);
ArmSmmu4KStatus ShengGpuSmmuObserve(const ArmSmmu4KIo *,uint64_t register_physical,
    const ShengGpuSmmuSession *,ShengGpuSmmuResult *);
/* Same pinned stream/context lifecycle, using the actual reserved-arena
 * auditor rather than the compact nonempty-tree auditor. Start requires an
 * already prepared/cache-published EMPTY arena and a single enabled region:
 * GPU upper TTBR1, GMU lower TTBR0. Config/result pointers must be the arena's
 * actual config and state's tables. Caller retains the real allocator-backed
 * pool, metadata and native device reference throughout. All platform power,
 * ownership, cache, fault IRQ and quiescence preconditions above still apply.
 * ObserveLive receives all CURRENT mapped extents after successful updates;
 * it verifies actual streams/context before and after the physical walk.
 * No context/address/stream is synthesized and no live-table copy is made.
 * Startup/stop uncertainty poisons the arena as well as the board session. */
ArmSmmu4KStatus ShengGpuSmmuStartLive(const ArmSmmu4KIo *,uint64_t register_physical,
    ShengGpuSmmuRole,unsigned bank,const unsigned *slots,unsigned count,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KLiveArena *,ArmSmmu4KLiveState *,
    unsigned poll_budget,ShengGpuSmmuSession *,ShengGpuSmmuResult *);
ArmSmmu4KStatus ShengGpuSmmuObserveLive(const ArmSmmu4KIo *,uint64_t register_physical,
    const ShengGpuSmmuSession *,const ArmSmmu4KRange *ranges,size_t count,ShengGpuSmmuResult *);
/* Requires caller-proven stopped masters. Fault all owned streams BEFORE
 * disabling the context; retain every resource on success and failure. */
ArmSmmu4KStatus ShengGpuSmmuStop(const ArmSmmu4KIo *,uint64_t register_physical,
    unsigned poll_budget,ShengGpuSmmuSession *,ShengGpuSmmuResult *);
#ifdef __cplusplus
}
#endif
#endif

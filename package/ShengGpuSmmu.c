/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengGpuSmmu.h"
#include "ShengGpuSmmuSource.h"
static int mapped(const ArmSmmu4KIo *io,uint64_t physical)
{return io && physical==SHENG_GPU_SMMU_BASE && io->register_bytes==SHENG_GPU_SMMU_BYTES;}
static void result_init(ShengGpuSmmuResult *r)
{*r=(ShengGpuSmmuResult){0};r->must_retain_resources=1;r->cleanup_status=ArmSmmu4KBusy;}
static void poison(ShengGpuSmmuSession *s)
{s->poisoned=1;if(s->live_state)s->live_state->poisoned=1;}
static ArmSmmu4KStatus observe(const ArmSmmu4KIo *io,const ShengGpuSmmuSession *s,
    const ArmSmmu4KRange *ranges,size_t count,ShengGpuSmmuResult *r)
{
    ArmSmmu4KStatus status=ArmSmmu4KObserveStreams(io,&s->streams,&s->context,&r->streams);if(status)return status;
    if(s->live_arena) {
        ArmSmmu4KProgramResult before;
        status=ArmSmmu4KObserveOwnedContext(io,s->streams.bank,&s->context,&before);if(status)return status;
        status=ArmSmmu4KLiveAudit(s->live_arena,s->live_state,ranges,count,&r->live);if(status)return status;
        status=ArmSmmu4KObserveOwnedContext(io,s->streams.bank,&s->context,&r->context);if(status)return status;
        if(before.id0!=r->context.id0 || before.id1!=r->context.id1 || before.id2!=r->context.id2 ||
            before.context_offset!=r->context.context_offset)return ArmSmmu4KStateMismatch;
    } else {
        status=ArmSmmu4KObserveAuditedContext(io,s->streams.bank,&s->context,&s->audit,&r->audit);if(status)return status;
    }
    return ArmSmmu4KObserveStreams(io,&s->streams,&s->context,&r->streams);
}
static ArmSmmu4KStatus start(const ArmSmmu4KIo *io,uint64_t physical,
    ShengGpuSmmuRole role,unsigned bank,const unsigned *slots,unsigned n,
    const ArmSmmu4KContextConfig *c,const ArmSmmu4KAuditConfig *audit,
    const ArmSmmu4KLiveArena *arena,ArmSmmu4KLiveState *state,
    unsigned budget,ShengGpuSmmuSession *s,ShengGpuSmmuResult *r)
{
    if(!r)return ArmSmmu4KArgument;result_init(r);
    if(!mapped(io,physical) || !c || !s || !slots || !budget || budget>1048576 ||
        (!!arena!=!!state) || (!!audit==!!arena) ||
        c->continue_after_fault!=1 || (role!=ShengGpuSmmuGpu && role!=ShengGpuSmmuGmu) ||
        (role==ShengGpuSmmuGpu?(bank!=0 || n!=SHENG_GPU_SMMU_GPU_STREAMS || !c->upper):
            (!bank || n!=1 || !c->lower || c->upper)))return ArmSmmu4KArgument;
    if(s->attempted || s->ready || s->poisoned)return ArmSmmu4KBusy;
    ArmSmmu4KStatus status;
    if(arena) {
        if(state->tables.leaf_pages || !state->prepared || state->poisoned ||
            (role==ShengGpuSmmuGpu ? (c->lower || c->lower_tables || arena->upper_region!=1 ||
                c->upper!=&arena->config || c->upper_tables!=&state->tables):
                (c->upper_tables || arena->upper_region || c->lower!=&arena->config || c->lower_tables!=&state->tables)))return ArmSmmu4KArgument;
        status=ArmSmmu4KLiveAudit(arena,state,0,0,&r->live);
    } else status=ArmSmmu4KAuditDeclaredContext(c,audit,&r->audit);
    if(status)return status;
    ArmSmmu4KStream streams[2];
    for(unsigned i=0;i<n;++i)streams[i]=(ArmSmmu4KStream){slots[i],role==ShengGpuSmmuGpu?sheng_gpu_smmu_sids[i]:SHENG_GPU_SMMU_GMU_SID};
    r->phase=1;status=ArmSmmu4KCheckFaultedStreams(io,streams,n,bank,&r->streams);if(status)return status;
    s->attempted=1;s->role=role;s->context=*c;
    s->audit=audit?*audit:(ArmSmmu4KAuditConfig){0};s->live_arena=arena;s->live_state=state;
    r->phase=2;status=ArmSmmu4KProgramDisabledContext(io,bank,&s->context,budget,&r->context);
    if(status){poison(s);return status;}
    r->phase=3;status=ArmSmmu4KAttachStreams(io,&s->streams,streams,n,bank,&s->context,&r->streams);
    if(!status){r->phase=11;status=observe(io,s,0,0,r);}
    if(status) {
        poison(s);
        ArmSmmu4KStreamResult check;
        if(s->streams.attached) {
            r->cleanup_status=ArmSmmu4KFaultStreams(io,&s->streams,&check);
            if(r->cleanup_status){r->phase=5;return status;}
        }
        r->cleanup_status=ArmSmmu4KCheckFaultedStreams(io,streams,n,bank,&check);
        if(!r->cleanup_status)r->cleanup_status=ArmSmmu4KDisableOwnedContext(io,bank,&s->context,budget,&r->context);
        r->phase=5;return status;
    }
    s->ready=1;r->phase=4;return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ShengGpuSmmuStart(const ArmSmmu4KIo *io,uint64_t physical,
    ShengGpuSmmuRole role,unsigned bank,const unsigned *slots,unsigned n,
    const ArmSmmu4KContextConfig *c,const ArmSmmu4KAuditConfig *audit,unsigned budget,
    ShengGpuSmmuSession *s,ShengGpuSmmuResult *r)
{return start(io,physical,role,bank,slots,n,c,audit,0,0,budget,s,r);}
ArmSmmu4KStatus ShengGpuSmmuStartLive(const ArmSmmu4KIo *io,uint64_t physical,
    ShengGpuSmmuRole role,unsigned bank,const unsigned *slots,unsigned n,
    const ArmSmmu4KContextConfig *c,const ArmSmmu4KLiveArena *arena,ArmSmmu4KLiveState *state,
    unsigned budget,ShengGpuSmmuSession *s,ShengGpuSmmuResult *r)
{return start(io,physical,role,bank,slots,n,c,0,arena,state,budget,s,r);}
ArmSmmu4KStatus ShengGpuSmmuObserve(const ArmSmmu4KIo *io,uint64_t physical,
    const ShengGpuSmmuSession *s,ShengGpuSmmuResult *r)
{
    if(!r)return ArmSmmu4KArgument;result_init(r);
    if(!mapped(io,physical) || !s || !s->ready || s->poisoned || s->live_arena)return ArmSmmu4KArgument;
    r->phase=6;return observe(io,s,0,0,r);
}
ArmSmmu4KStatus ShengGpuSmmuObserveLive(const ArmSmmu4KIo *io,uint64_t physical,
    const ShengGpuSmmuSession *s,const ArmSmmu4KRange *ranges,size_t count,ShengGpuSmmuResult *r)
{
    if(!r)return ArmSmmu4KArgument;result_init(r);
    if(!mapped(io,physical) || !s || !s->ready || s->poisoned || !s->live_arena || !s->live_state)
        return ArmSmmu4KArgument;
    r->phase=12;return observe(io,s,ranges,count,r);
}
ArmSmmu4KStatus ShengGpuSmmuStop(const ArmSmmu4KIo *io,uint64_t physical,
    unsigned budget,ShengGpuSmmuSession *s,ShengGpuSmmuResult *r)
{
    if(!r)return ArmSmmu4KArgument;result_init(r);
    if(!mapped(io,physical) || !s || !s->ready || s->poisoned || !budget || budget>1048576)return ArmSmmu4KArgument;
    r->phase=7;ArmSmmu4KStatus status=ArmSmmu4KFaultStreams(io,&s->streams,&r->streams);
    if(!status){r->phase=8;status=ArmSmmu4KDisableOwnedContext(io,s->streams.bank,&s->context,budget,&r->context);}
    if(status)poison(s);
    s->ready=0;r->phase=status?9:10;return status;
}

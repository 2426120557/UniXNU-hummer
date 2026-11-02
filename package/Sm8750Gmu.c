/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8750Gmu.h"
#include "AdrenoFirmware.h"
/* Register facts and local firmware pin; no vendor payload or AOP sequencer. */
#include "Sm8750GmuSource.h"
#if (!defined(__aarch64__) && !defined(SM8750_GMU_HOST_TEST)) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error SM8750 GMU requires little-endian AArch64
#endif
static int extent(uintptr_t at,size_t bytes)
{return at && bytes && bytes-1<=UINTPTR_MAX-at;}
static int overlap(uintptr_t a,size_t na,uintptr_t b,size_t nb)
{return a<=b?(b-a<na):(a-b<nb);}
static size_t core_offset(uint32_t word)
{return ((size_t)word<<2)-SM8750_GMU_CORE_GPU_DELTA;}
static int read32(Sm8750GmuDevice *d,unsigned window,size_t at,uint32_t *v,Sm8750GmuResult *r)
{++r->read_attempts;return d->io.read32(d->io.cookie,window,at,v);}
static int write32(Sm8750GmuDevice *d,unsigned window,size_t at,uint32_t v,Sm8750GmuResult *r)
{++r->write_attempts;return d->io.write32(d->io.cookie,window,at,v);}
static int core_read(Sm8750GmuDevice *d,uint32_t word,uint32_t *v,Sm8750GmuResult *r)
{return read32(d,Sm8750GmuCore,core_offset(word),v,r);}
static int core_write(Sm8750GmuDevice *d,uint32_t word,uint32_t v,Sm8750GmuResult *r)
{return write32(d,Sm8750GmuCore,core_offset(word),v,r);}
static Sm8750GmuStatus check_faults(Sm8750GmuDevice *d,Sm8750GmuResult *r)
{
    if(core_read(d,REG_GMU_AO_HOST_INTERRUPT_STATUS,&r->ao_fault_status,r) ||
        core_read(d,REG_GMU_GMU2HOST_INTR_INFO,&r->cm3_fault_status,r))return Sm8750GmuIoError;
    return (r->ao_fault_status&SM8750_GMU_AO_FATAL_MASK) ||
        (r->cm3_fault_status&SM8750_GMU_CM3_FATAL_MASK)?Sm8750GmuFault:Sm8750GmuSuccess;
}
static int notify(void *cookie)
{
    Sm8750GmuDevice *d=cookie;
    return d->io.barrier(d->io.cookie) ||
        d->io.write32(d->io.cookie,Sm8750GmuCore,core_offset(REG_GMU_HOST2GMU_INTR_SET),1) ||
        d->io.barrier(d->io.cookie);
}
static Sm8750GmuStatus poll(Sm8750GmuDevice *d,uint32_t reg,uint32_t mask,
    uint32_t expected,unsigned budget,uint32_t *observed,Sm8750GmuResult *r)
{
    uint64_t start,last,now;
    if(d->services.now_us(d->services.cookie,&start))return Sm8750GmuClockError;
    last=start;
    for(unsigned i=0;i<budget;++i) {
        if(d->services.now_us(d->services.cookie,&now) || now<last)return Sm8750GmuClockError;
        if(now-start>=100000)return Sm8750GmuTimeout;
        last=now;++r->poll_reads;
        Sm8750GmuStatus fault=check_faults(d,r);if(fault!=Sm8750GmuSuccess)return fault;
        if(core_read(d,reg,observed,r))return Sm8750GmuIoError;
        if((*observed&mask)==expected) {
            if(d->io.barrier(d->io.cookie))return Sm8750GmuIoError;
            return Sm8750GmuSuccess;
        }
        if(i+1<budget && d->services.wait_us(d->services.cookie,100))return Sm8750GmuClockError;
    }
    return Sm8750GmuTimeout;
}
static void reset_after_failure(Sm8750GmuDevice *d,Sm8750GmuResult *r)
{
    uint32_t actual;r->reset_verified=0;d->poisoned=1;d->hfi.poisoned=1;
    if(!core_write(d,REG_GMU_CM3_SYSRESET,1,r) && !d->io.barrier(d->io.cookie) &&
        !core_read(d,REG_GMU_CM3_SYSRESET,&actual,r) && actual==1)r->reset_verified=1;
}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static Sm8750GmuStatus validate(Sm8750GmuDevice *d,const Sm8750GmuConfig *c,Sm8750GmuResult *r)
{
    if(!d || !c || !d->io.read32 || !d->io.write32 || !d->io.barrier ||
        !d->services.sha256 || !d->services.now_us || !d->services.wait_us ||
        c->chip_id!=SM8750_GMU_CHIP_ID || !c->firmware || c->firmware_bytes!=SM8750_GMU_FIRMWARE_BYTES ||
        !extent((uintptr_t)c->firmware,c->firmware_bytes) || !c->poll_budget || c->poll_budget>1024)
        return Sm8750GmuArgument;
    if(d->poisoned)return Sm8750GmuPoisoned;
    if(d->boot_attempted)return Sm8750GmuBusy;
    for(unsigned i=0;i<Sm8750GmuWindows;++i)if(d->io.bytes[i]!=sm8750_gmu_window_bytes[i])return Sm8750GmuArgument;
    const Sm8750GmuMemory *mem[]={&c->icache,&c->dummy,&c->debug,&c->log,&c->hfi};
    const size_t sizes[]={0xffc000,0x4000,0x7000,0x4000,0x4000};
    for(unsigned i=0;i<5;++i) {
        const Sm8750GmuMemory *m=mem[i];
        if(m->bytes!=sizes[i] || !extent((uintptr_t)m->cpu,m->bytes) || ((uintptr_t)m->cpu&4095) ||
            !m->iova || (m->iova&4095) || m->iova>UINT32_MAX-(m->bytes-1) ||
            overlap((uintptr_t)m->cpu,m->bytes,(uintptr_t)c->firmware,c->firmware_bytes))return Sm8750GmuArgument;
        for(unsigned j=0;j<i;++j)if(overlap((uintptr_t)m->cpu,m->bytes,(uintptr_t)mem[j]->cpu,mem[j]->bytes) ||
            overlap((uintptr_t)m->iova,m->bytes,(uintptr_t)mem[j]->iova,mem[j]->bytes))return Sm8750GmuArgument;
    }
    if(c->icache.iova!=0x4000 || c->dummy.iova!=0x60000000 || c->debug.iova!=0x60400000)return Sm8750GmuArgument;
    if(d->services.sha256(d->services.cookie,c->firmware,c->firmware_bytes,r->complete_sha256))return Sm8750GmuFirmware;
    for(unsigned i=0;i<32;++i)if(r->complete_sha256[i]!=sm8750_gmu_firmware_sha256[i])return Sm8750GmuFirmware;
    r->complete_hash_verified=1;return Sm8750GmuSuccess;
}
Sm8750GmuStatus Sm8750GmuLoadAndStartHfi(Sm8750GmuDevice *d,const Sm8750GmuConfig *c,Sm8750GmuResult *r)
{
    if(!r)return Sm8750GmuArgument;
    *r=(Sm8750GmuResult){0};r->resources_must_retain=1;
    Sm8750GmuStatus status=validate(d,c,r);if(status!=Sm8750GmuSuccess)return status;
    const AdrenoFirmwareRegion regions[]={{0,0x4000,1},{0x4000,0xffc000,2},{0x10004000,0x4000,3}};
    AdrenoFirmwareBlock blocks[32];size_t count=0;
    if(AdrenoFirmwarePlanGmu(c->firmware,c->firmware_bytes,regions,3,blocks,32,&count)!=AdrenoFirmwareSuccess || count!=15)
        return Sm8750GmuFirmware;
    r->phase=1;uint32_t value;
    if(core_read(d,REG_GMU_CM3_SYSRESET,&value,r))return Sm8750GmuIoError;
    if(value!=1)return Sm8750GmuBusy;
    r->reset_verified=1;
    if(read32(d,Sm8750GmuRscc,REG_RSCC_SEQ_BUSY_DRV0*4,&value,r))return Sm8750GmuIoError;
    if(value)return Sm8750GmuBusy;
    if(core_read(d,REG_GMU_HFI_CTRL_STATUS,&value,r))return Sm8750GmuIoError;
    if(value&1)return Sm8750GmuStateMismatch; /* A stale ready bit cannot pass. */
    status=check_faults(d,r);if(status!=Sm8750GmuSuccess)return status;
    r->phase=2;
    d->boot_attempted=1;
    /* Gen8 RSC/PDC sequences and TCM retention belong to AOP. */
    if(core_write(d,REG_GPU_GMU_CX_GMU_CX_FALNEXT_INTF,1,r) ||
        core_write(d,REG_GPU_GMU_CX_GMU_CX_FAL_INTF,1,r) || d->io.barrier(d->io.cookie)) {
        status=Sm8750GmuIoError;goto failed;
    }
    /* All shared allocations are real owned coherent memory, not simulated
     * GMU addresses. Zero only after actual stopped/idle checks above. */
    const Sm8750GmuMemory *memory[]={&c->icache,&c->dummy,&c->debug,&c->log};
    r->memory_changed=1;
    for(unsigned m=0;m<4;++m)for(size_t i=0;i<memory[m]->bytes/4;++i)memory[m]->cpu[i]=0;
    if(AdrenoHfiInitializeStopped(&d->hfi,c->hfi.cpu,c->hfi.bytes,c->hfi.iova,d,notify)!=AdrenoHfiSuccess) {
        status=Sm8750GmuArgument;goto failed;
    }
    r->phase=3;
    for(size_t i=0;i<count;++i) {
        const AdrenoFirmwareBlock *b=blocks+i;
        /* Zero-size records authorize no allocation or write. The pinned
         * loader skips them; all real extents were independently supplied. */
        if(!b->bytes)continue;
        for(size_t at=0;at<b->bytes;at+=4) {
            uint32_t word=le32(c->firmware+b->payload_offset+at);
            if(b->region_identifier==2)c->icache.cpu[(b->address-0x4000+at)/4]=word;
            else {
                uint32_t reg=b->region_identifier==1?REG_GMU_CM3_ITCM_START+b->address/4:
                    REG_GMU_CM3_DTCM_START+(b->address-0x10004000)/4;
                if(core_write(d,reg+(uint32_t)(at/4),word,r)) {status=Sm8750GmuIoError;goto failed;}
            }
        }
    }
    if(d->io.barrier(d->io.cookie)) {status=Sm8750GmuIoError;goto failed;}
    /* Read every TCM payload word before releasing reset. CPU cache readback
     * checks copied bytes only; actual GMU coherency is still a prerequisite. */
    for(size_t i=0;i<count;++i) {
        const AdrenoFirmwareBlock *b=blocks+i;
        for(size_t at=0;at<b->bytes;at+=4) {
            if(b->region_identifier==2)value=c->icache.cpu[(b->address-0x4000+at)/4];
            else {
                uint32_t reg=b->region_identifier==1?REG_GMU_CM3_ITCM_START+b->address/4:
                    REG_GMU_CM3_DTCM_START+(b->address-0x10004000)/4;
                if(core_read(d,reg+(uint32_t)(at/4),&value,r)) {status=Sm8750GmuIoError;goto failed;}
            }
            if(value!=le32(c->firmware+b->payload_offset+at)) {status=Sm8750GmuStateMismatch;goto failed;}
        }
    }
    r->phase=4;
    if(core_write(d,REG_GMU_CM3_FW_INIT_RESULT,0,r) ||
        core_write(d,REG_GMU_CM3_BOOT_CONFIG,2,r) ||
        core_write(d,REG_GMU_HFI_QTBL_ADDR,(uint32_t)c->hfi.iova,r) ||
        core_write(d,REG_GMU_HFI_QTBL_INFO,1,r) ||
        core_write(d,REG_GMU_AHB_FENCE_RANGE_0,UINT32_C(0x80000000)|(0x32u<<18)|0x8c0,r) ||
        core_write(d,REG_GMU_CM3_CFG,0x4052,r) ||
        core_write(d,REG_GMU_GENERAL_10,SM8750_GMU_FW_CHIP_ID,r) ||
        core_write(d,REG_GMU_GENERAL_8,(uint32_t)c->log.iova|3,r)) {status=Sm8750GmuIoError;goto failed;}
    for(unsigned i=0;i<4;++i)if(write32(d,Sm8750GmuGpu,(REG_GBIF_QSB_SIDE0+i)*4,0x71e20,r)) {status=Sm8750GmuIoError;goto failed;}
    if(write32(d,Sm8750GmuGpu,REG_GBIF_CX_CONFIG*4,0x20023000,r) ||
        core_write(d,REG_GMU_MRC_GBIF_QOS_CTRL,0x33,r)) {status=Sm8750GmuIoError;goto failed;}
    if(core_write(d,REG_GMU_SYS_BUS_CONFIG,1,r) || core_write(d,REG_GMU_ICACHE_CONFIG,1,r) ||
        core_write(d,REG_GMU_DCACHE_CONFIG,1,r) || core_read(d,REG_GMU_CM3_DTCM_START+0xff8,&value,r)) {status=Sm8750GmuIoError;goto failed;}
    uint32_t mask=value<=UINT32_C(0x20010004)?UINT32_MAX:0x1ff;
    uint32_t ready=mask==UINT32_MAX?UINT32_C(0xbabeface):0x100;
    if(core_write(d,REG_GMU_CM3_SYSRESET,1,r) || core_write(d,REG_GMU_GENERAL_9,0,r) ||
        d->io.barrier(d->io.cookie)) {status=Sm8750GmuIoError;goto failed;}
    const uint32_t config_regs[]={REG_GMU_CM3_FW_INIT_RESULT,REG_GMU_CM3_BOOT_CONFIG,
        REG_GMU_HFI_QTBL_ADDR,REG_GMU_HFI_QTBL_INFO,REG_GMU_CM3_CFG,REG_GMU_GENERAL_10,
        REG_GMU_GENERAL_8,REG_GMU_AHB_FENCE_RANGE_0,REG_GMU_SYS_BUS_CONFIG,
        REG_GMU_ICACHE_CONFIG,REG_GMU_DCACHE_CONFIG};
    const uint32_t config_values[]={0,2,(uint32_t)c->hfi.iova,1,0x4052,SM8750_GMU_FW_CHIP_ID,
        (uint32_t)c->log.iova|3,UINT32_C(0x80000000)|(0x32u<<18)|0x8c0,1,1,1};
    for(unsigned i=0;i<sizeof(config_regs)/sizeof(config_regs[0]);++i) {
        if(core_read(d,config_regs[i],&value,r)) {status=Sm8750GmuIoError;goto failed;}
        if(value!=config_values[i]) {status=Sm8750GmuStateMismatch;goto failed;}
    }
    r->reset_verified=0;
    if(core_write(d,REG_GMU_CM3_SYSRESET,0,r) || d->io.barrier(d->io.cookie)) {status=Sm8750GmuIoError;goto failed;}
    r->phase=5;
    status=poll(d,REG_GMU_CM3_FW_INIT_RESULT,mask,ready,c->poll_budget,&r->firmware_init_result,r);
    if(status!=Sm8750GmuSuccess)goto failed;
    r->firmware_ready_observed=1;
    if(core_write(d,REG_GMU_HFI_CTRL_INIT,1,r) || d->io.barrier(d->io.cookie)) {status=Sm8750GmuIoError;goto failed;}
    r->phase=6;
    status=poll(d,REG_GMU_HFI_CTRL_STATUS,1,1,c->poll_budget,&r->hfi_status,r);
    if(status!=Sm8750GmuSuccess)goto failed;
    status=check_faults(d,r);if(status!=Sm8750GmuSuccess)goto failed;
    if(d->io.barrier(d->io.cookie)) {status=Sm8750GmuIoError;goto failed;}
    r->hfi_ready_observed=1;r->phase=7;return Sm8750GmuSuccess;
failed:
    reset_after_failure(d,r);return status;
}
typedef struct {
    Sm8750GmuDevice *device;
    Sm8750GmuStartupResult *result;
} StartupContext;
static int startup_now(void *cookie,uint64_t *value)
{
    Sm8750GmuDevice *d=((StartupContext *)cookie)->device;
    return d->services.now_us(d->services.cookie,value);
}
static int startup_wait(void *cookie,unsigned us)
{
    Sm8750GmuDevice *d=((StartupContext *)cookie)->device;
    return d->services.wait_us(d->services.cookie,us);
}
static int startup_health(void *cookie)
{
    StartupContext *c=cookie;Sm8750GmuDevice *d=c->device;
    Sm8750GmuStartupResult *r=c->result;uint32_t reset,status;
    r->health_status=check_faults(d,&r->load);
    if(r->health_status!=Sm8750GmuSuccess)return 1;
    if(core_read(d,REG_GMU_CM3_SYSRESET,&reset,&r->load) ||
        core_read(d,REG_GMU_HFI_CTRL_STATUS,&status,&r->load))r->health_status=Sm8750GmuIoError;
    else if(reset!=0 || !(status&1))r->health_status=Sm8750GmuStateMismatch;
    return r->health_status!=Sm8750GmuSuccess;
}
Sm8750GmuStatus Sm8750GmuColdStart(Sm8750GmuDevice *d,const Sm8750GmuConfig *c,
    const AdrenoHfiStartupConfig *config,const Sm8750GmuStartupLimits *limits,Sm8750GmuStartupResult *r)
{
    if(!r)return Sm8750GmuArgument;
    *r=(Sm8750GmuStartupResult){0};r->load.resources_must_retain=r->protocol.resources_must_retain=1;
    r->protocol_status=AdrenoHfiStartupInvalid; /* Not attempted until the load succeeds. */
    if(!d || !c || !limits || !config || config->performance_table!=AdrenoHfiPerfA8xx ||
        AdrenoHfiStartupValidate(config)!=AdrenoHfiStartupSuccess)return Sm8750GmuArgument;
    StartupContext context={d,r};
    AdrenoHfiStartupServices services={&context,startup_now,startup_wait,startup_health,
        limits->polls_per_command,limits->wait_interval_us,limits->command_timeout_us,limits->total_timeout_us};
    if(AdrenoHfiStartupValidateServices(&services)!=AdrenoHfiStartupSuccess)return Sm8750GmuArgument;
    if(d->poisoned || d->startup.poisoned)return Sm8750GmuPoisoned;
    if(d->startup.attempted || d->startup.completed)return Sm8750GmuBusy;
    Sm8750GmuStatus status=Sm8750GmuLoadAndStartHfi(d,c,&r->load);
    if(status!=Sm8750GmuSuccess)return status;
    r->protocol_status=AdrenoHfiStartupRun(&d->startup,&d->hfi,config,&services,&r->protocol);
    if(r->protocol_status==AdrenoHfiStartupSuccess) {
        d->gpu_levels=config->gpu_count;d->bandwidth_levels=config->bandwidth_count;
        return Sm8750GmuSuccess;
    }
    switch(r->protocol_status) {
    case AdrenoHfiStartupClockError:case AdrenoHfiStartupWaitError:status=Sm8750GmuClockError;break;
    case AdrenoHfiStartupTimeout:status=Sm8750GmuTimeout;break;
    case AdrenoHfiStartupPlatformFault:status=r->health_status;break;
    default:status=Sm8750GmuProtocolError;break;
    }
    reset_after_failure(d,&r->load);d->startup.poisoned=1;
    return status;
}
static Sm8750GmuStatus runtime_health(Sm8750GmuDevice *d,Sm8750GmuResult *r)
{
    if(!d || !r || !d->io.read32 || !d->io.write32 || !d->io.barrier ||
        !d->services.now_us || !d->services.wait_us)return Sm8750GmuArgument;
    if(d->poisoned || d->hfi.poisoned || d->startup.poisoned)return Sm8750GmuPoisoned;
    if(!d->startup.completed)return Sm8750GmuStateMismatch;
    uint32_t reset,hfi;
    Sm8750GmuStatus status=check_faults(d,r);
    if(status!=Sm8750GmuSuccess)return status;
    if(core_read(d,REG_GMU_CM3_SYSRESET,&reset,r) || core_read(d,REG_GMU_HFI_CTRL_STATUS,&hfi,r))return Sm8750GmuIoError;
    return reset || !(hfi&1)?Sm8750GmuStateMismatch:Sm8750GmuSuccess;
}
static Sm8750GmuStatus runtime_poison(Sm8750GmuDevice *d,Sm8750GmuStatus status)
{d->poisoned=d->hfi.poisoned=d->startup.poisoned=1;return status;}
Sm8750GmuStatus Sm8750GmuGpuOob(Sm8750GmuDevice *d,unsigned acquire,Sm8750GmuResult *r)
{
    if(!r || !d || acquire>1)return Sm8750GmuArgument;
    *r=(Sm8750GmuResult){0};r->resources_must_retain=1;
    Sm8750GmuStatus status=runtime_health(d,r);
    if(status!=Sm8750GmuSuccess)return status;
    if(acquire==d->gpu_oob_held)return Sm8750GmuBusy;
    const uint32_t ack=UINT32_C(1)<<31;uint32_t observed;
    if(acquire) {
        if(core_write(d,REG_GMU_GMU2HOST_INTR_CLR,ack,r) || d->io.barrier(d->io.cookie) ||
            core_read(d,REG_GMU_GMU2HOST_INTR_INFO,&observed,r))return runtime_poison(d,Sm8750GmuIoError);
        if(observed&ack)return runtime_poison(d,Sm8750GmuStateMismatch);
    }
    if(core_write(d,REG_GMU_HOST2GMU_INTR_SET,acquire?UINT32_C(1)<<30:ack,r) ||
        d->io.barrier(d->io.cookie))return runtime_poison(d,Sm8750GmuIoError);
    if(acquire) {
        status=poll(d,REG_GMU_GMU2HOST_INTR_INFO,ack,ack,1000,&observed,r);
        if(status!=Sm8750GmuSuccess)return runtime_poison(d,status);
        if(core_write(d,REG_GMU_GMU2HOST_INTR_CLR,ack,r) || d->io.barrier(d->io.cookie))
            return runtime_poison(d,Sm8750GmuIoError);
    }
    d->gpu_oob_held=acquire;return Sm8750GmuSuccess;
}
Sm8750GmuStatus Sm8750GmuSetPerformance(Sm8750GmuDevice *d,unsigned frequency,unsigned bandwidth,Sm8750GmuResult *r)
{
    if(!d || !r || !frequency || frequency>=d->gpu_levels || !bandwidth || bandwidth>=d->bandwidth_levels)
        return Sm8750GmuArgument;
    *r=(Sm8750GmuResult){0};r->resources_must_retain=1;
    Sm8750GmuStatus status=runtime_health(d,r);
    if(status!=Sm8750GmuSuccess)return status;
    uint64_t start,last,now;
    if(d->services.now_us(d->services.cookie,&start))return Sm8750GmuClockError;
    last=start;
    uint32_t payload[]={1,frequency,bandwidth},header;
    AdrenoHfiStatus transport=AdrenoHfiSend(&d->hfi,30,payload,3,&header);
    if(transport!=AdrenoHfiSuccess)return runtime_poison(d,Sm8750GmuProtocolError);
    for(unsigned i=0;i<1000;++i) {
        if(d->services.now_us(d->services.cookie,&now) || now<last)return runtime_poison(d,Sm8750GmuClockError);
        if(now-start>=100000)return runtime_poison(d,Sm8750GmuTimeout);
        last=now;
        status=runtime_health(d,r);if(status!=Sm8750GmuSuccess)return runtime_poison(d,status);
        uint32_t reply[16],error;size_t count;
        transport=AdrenoHfiPoll(&d->hfi,reply,16,&count,&error);++r->poll_reads;
        if(transport==AdrenoHfiSuccess)return Sm8750GmuSuccess;
        if(transport!=AdrenoHfiEmpty)return runtime_poison(d,Sm8750GmuProtocolError);
        if(d->services.wait_us(d->services.cookie,100))return runtime_poison(d,Sm8750GmuClockError);
    }
    return runtime_poison(d,Sm8750GmuTimeout);
}
static int fits(Sm8750GmuMmio *m,unsigned window,size_t at)
{
    if(!m || window>=Sm8750GmuWindows)return 0;
    Sm8750GmuMmioWindow *w=m->windows+window;
    return extent((uintptr_t)w->cpu,w->bytes) && !((uintptr_t)w->cpu&3) && !(at&3) &&
        at<=w->bytes && w->bytes-at>=4;
}
static int mmio_read(void *cookie,unsigned window,size_t at,uint32_t *v)
{Sm8750GmuMmio *m=cookie;if(!v || !fits(m,window,at))return 1;*v=*(volatile uint32_t *)(m->windows[window].cpu+at);return 0;}
static int mmio_write(void *cookie,unsigned window,size_t at,uint32_t v)
{Sm8750GmuMmio *m=cookie;if(!fits(m,window,at))return 1;*(volatile uint32_t *)(m->windows[window].cpu+at)=v;return 0;}
static int mmio_barrier(void *cookie)
{(void)cookie;
#if defined(__aarch64__)
    __asm__ volatile("dsb sy":::"memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    return 0;
}
Sm8750GmuStatus Sm8750GmuBindMmio(Sm8750GmuMmio *m,Sm8750GmuIo *out)
{
    if(!out)return Sm8750GmuArgument;
    *out=(Sm8750GmuIo){0};
    if(!m)return Sm8750GmuArgument;
    for(unsigned i=0;i<Sm8750GmuWindows;++i) {
        const Sm8750GmuMmioWindow *w=m->windows+i;
        if(!fits(m,i,0) || w->physical!=sm8750_gmu_window_physical[i] ||
            w->bytes!=sm8750_gmu_window_bytes[i])return Sm8750GmuArgument;
        for(unsigned j=0;j<i;++j)if(overlap((uintptr_t)w->cpu,w->bytes,(uintptr_t)m->windows[j].cpu,m->windows[j].bytes))return Sm8750GmuArgument;
    }
    out->cookie=m;out->read32=mmio_read;out->write32=mmio_write;out->barrier=mmio_barrier;
    for(unsigned i=0;i<Sm8750GmuWindows;++i)out->bytes[i]=m->windows[i].bytes;
    return Sm8750GmuSuccess;
}

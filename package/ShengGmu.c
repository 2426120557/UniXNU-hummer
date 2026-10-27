/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8750 / Adreno 830 (gen8_0_0) GMU cold start.
 *
 * Structurally the same loader the SM8550 board uses (same register-level
 * order, same state checks before every mutation), retargeted with the
 * A8XX offsets and behaviours documented in ShengGmuSource.h.  The three
 * places where A8XX genuinely differs from A7XX are called out inline:
 *   - a6xx_gmu.c returns from a6xx_gmu_rpmh_init() before any RSC/PDC write
 *     because AOP owns that configuration, so the setup table is empty and no
 *     PDC window exists.
 *   - a6xx_gmu.c writes CX_MISC_TCM_RET_CNTL for A7XX only; A8XX neither
 *     writes nor reads it, so the cx_misc window stays untouched here.
 *   - the CX GBIF programming comes from the pinned a840_gbif[] list instead of
 *     a610's GBIF_QSB_SIDE value.
 */
#include "ShengGmu.h"
#include "AdrenoFirmware.h"
#include "ShengGmuSource.h"
#if !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error sheng GMU requires little-endian AArch64
#endif
static int extent(uintptr_t at,size_t bytes)
{return at && bytes && bytes-1<=UINTPTR_MAX-at;}
static int overlap(uintptr_t a,size_t na,uintptr_t b,size_t nb)
{return a<=b?(b-a<na):(a-b<nb);}
static size_t core_offset(uint32_t word)
{return ((size_t)word<<2)-SHENG_GMU_CORE_GPU_DELTA;}
static int read32(ShengGmuDevice *d,unsigned window,size_t at,uint32_t *v,ShengGmuResult *r)
{++r->read_attempts;return d->io.read32(d->io.cookie,window,at,v);}
static int write32(ShengGmuDevice *d,unsigned window,size_t at,uint32_t v,ShengGmuResult *r)
{++r->write_attempts;return d->io.write32(d->io.cookie,window,at,v);}
static int core_read(ShengGmuDevice *d,uint32_t word,uint32_t *v,ShengGmuResult *r)
{return read32(d,ShengGmuCore,core_offset(word),v,r);}
static int core_write(ShengGmuDevice *d,uint32_t word,uint32_t v,ShengGmuResult *r)
{return write32(d,ShengGmuCore,core_offset(word),v,r);}
static ShengGmuStatus check_faults(ShengGmuDevice *d,ShengGmuResult *r)
{
    if(core_read(d,REG_GMU_AO_HOST_INTERRUPT_STATUS,&r->ao_fault_status,r) ||
        core_read(d,REG_GMU_GMU2HOST_INTR_INFO,&r->cm3_fault_status,r))return ShengGmuIoError;
    return (r->ao_fault_status&SHENG_GMU_AO_FATAL_MASK) ||
        (r->cm3_fault_status&SHENG_GMU_CM3_FATAL_MASK)?ShengGmuFault:ShengGmuSuccess;
}
static int notify(void *cookie)
{
    ShengGmuDevice *d=cookie;
    return d->io.barrier(d->io.cookie) ||
        d->io.write32(d->io.cookie,ShengGmuCore,core_offset(REG_GMU_HOST2GMU_INTR_SET),1) ||
        d->io.barrier(d->io.cookie);
}
static ShengGmuStatus poll(ShengGmuDevice *d,uint32_t reg,uint32_t mask,
    uint32_t expected,unsigned budget,uint32_t *observed,ShengGmuResult *r)
{
    uint64_t start,last,now;
    if(d->services.now_us(d->services.cookie,&start))return ShengGmuClockError;
    last=start;
    for(unsigned i=0;i<budget;++i) {
        if(d->services.now_us(d->services.cookie,&now) || now<last)return ShengGmuClockError;
        /* 100 ms, the budget the annibale port gives its CM3 (adreno8_wait_for
         * ..., 100). 10 ms cut the poll off after 56 reads on v159. */
        if(now-start>=100000)return ShengGmuTimeout;
        last=now;++r->poll_reads;
        ShengGmuStatus fault=check_faults(d,r);if(fault!=ShengGmuSuccess)return fault;
        if(core_read(d,reg,observed,r))return ShengGmuIoError;
        if((*observed&mask)==expected) {
            if(d->io.barrier(d->io.cookie))return ShengGmuIoError;
            return ShengGmuSuccess;
        }
        if(i+1<budget && d->services.wait_us(d->services.cookie,100))return ShengGmuClockError;
    }
    return ShengGmuTimeout;
}
static void reset_after_failure(ShengGmuDevice *d,ShengGmuResult *r)
{
    uint32_t actual;r->reset_verified=0;d->poisoned=1;d->hfi.poisoned=1;
    if(!core_write(d,REG_GMU_CM3_SYSRESET,1,r) && !d->io.barrier(d->io.cookie) &&
        !core_read(d,REG_GMU_CM3_SYSRESET,&actual,r) && actual==1)r->reset_verified=1;
}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
/* SM8750 has no gmu_pdc resource: a6xx_gmu.c leaves RPMH/PDC to AOP on A8XX.
 * The window is therefore required to be absent rather than merely unused. */
static int window_size_ok(unsigned window,size_t bytes)
{
#if SHENG_GMU_PDC_PRESENT
    return bytes==sheng_gmu_window_bytes[window];
#else
    if(window==ShengGmuPdc)return bytes==0;
    return bytes==sheng_gmu_window_bytes[window];
#endif
}
static ShengGmuStatus validate(ShengGmuDevice *d,const ShengGmuConfig *c,ShengGmuResult *r)
{
    if(!d || !c || !d->io.read32 || !d->io.write32 || !d->io.barrier ||
        !d->services.sha256 || !d->services.now_us || !d->services.wait_us ||
        c->chip_id!=SHENG_GMU_CHIP_ID || !c->firmware || c->firmware_bytes!=SHENG_GMU_FIRMWARE_BYTES ||
        !extent((uintptr_t)c->firmware,c->firmware_bytes) || !c->poll_budget || c->poll_budget>1024)
        return ShengGmuArgument;
    if(d->poisoned)return ShengGmuPoisoned;
    if(d->boot_attempted)return ShengGmuBusy;
    for(unsigned i=0;i<ShengGmuWindows;++i)if(!window_size_ok(i,d->io.bytes[i]))return ShengGmuArgument;
    const ShengGmuMemory *mem[]={&c->icache,&c->dummy,&c->debug,&c->log,&c->hfi};
    /* a6xx_gmu_memory_init(): A8XX takes the a650/a7xx branch, i.e. the 16 MiB
     * icache (minus the 16 KiB below 0x4000), a 16 KiB dummy at 0x60000000,
     * 7 pages of debug at 0x60400000, and 16 KiB each for log and HFI. */
    const size_t sizes[]={0xffc000,0x4000,0x7000,0x4000,0x4000};
    for(unsigned i=0;i<5;++i) {
        const ShengGmuMemory *m=mem[i];
        if(m->bytes!=sizes[i] || !extent((uintptr_t)m->cpu,m->bytes) || ((uintptr_t)m->cpu&4095) ||
            !m->iova || (m->iova&4095) || m->iova>UINT32_MAX-(m->bytes-1) ||
            overlap((uintptr_t)m->cpu,m->bytes,(uintptr_t)c->firmware,c->firmware_bytes))return ShengGmuArgument;
        for(unsigned j=0;j<i;++j)if(overlap((uintptr_t)m->cpu,m->bytes,(uintptr_t)mem[j]->cpu,mem[j]->bytes) ||
            overlap((uintptr_t)m->iova,m->bytes,(uintptr_t)mem[j]->iova,mem[j]->bytes))return ShengGmuArgument;
    }
    if(c->icache.iova!=0x4000 || c->dummy.iova!=0x60000000 || c->debug.iova!=0x60400000)return ShengGmuArgument;
    if(d->services.sha256(d->services.cookie,c->firmware,c->firmware_bytes,r->complete_sha256))return ShengGmuFirmware;
    for(unsigned i=0;i<32;++i)if(r->complete_sha256[i]!=sheng_gmu_firmware_sha256[i])return ShengGmuFirmware;
    r->complete_hash_verified=1;return ShengGmuSuccess;
}
ShengGmuStatus ShengGmuLoadAndStartHfi(ShengGmuDevice *d,const ShengGmuConfig *c,ShengGmuResult *r)
{
    if(!r)return ShengGmuArgument;
    *r=(ShengGmuResult){0};r->resources_must_retain=1;
    ShengGmuStatus status=validate(d,c,r);if(status!=ShengGmuSuccess)return status;
    const AdrenoFirmwareRegion regions[]={{0,0x4000,1},{0x4000,0xffc000,2},{0x10004000,0x4000,3}};
    AdrenoFirmwareBlock blocks[32];size_t count=0;
    if(AdrenoFirmwarePlanGmu(c->firmware,c->firmware_bytes,regions,3,blocks,32,&count)!=AdrenoFirmwareSuccess || count!=15)
        return ShengGmuFirmware;
    r->phase=1;uint32_t value;
    if(core_read(d,REG_GMU_CM3_SYSRESET,&value,r))return ShengGmuIoError;
    if(value!=1)return ShengGmuBusy;
    r->reset_verified=1;
    if(read32(d,ShengGmuRscc,REG_RSCC_SEQ_BUSY_DRV0*4,&value,r))return ShengGmuIoError;
    if(value)return ShengGmuBusy;
    if(core_read(d,REG_GMU_HFI_CTRL_STATUS,&value,r))return ShengGmuIoError;
    if(value&1)return ShengGmuStateMismatch; /* A stale ready bit cannot pass. */
    status=check_faults(d,r);if(status!=ShengGmuSuccess)return status;
    r->phase=2;
    d->boot_attempted=1;
    /* Empty on A8XX (see the header): AOP owns RSC and PDC.  The loop is kept
     * so a board that does need setup writes can supply them without a code
     * change here. */
    for(unsigned i=0;i<SHENG_GMU_RSC_SETUP_COUNT;++i) {
        const ShengGmuSetupWrite *w=sheng_gmu_rsc_setup+i;
        if(write32(d,w->window,w->byte_offset,w->value,r)) {status=ShengGmuIoError;goto failed;}
    }
    if(d->io.barrier(d->io.cookie)) {status=ShengGmuIoError;goto failed;}
    /* A7XX raised the FAL votes and enabled TCM retention here.  a6xx_gmu.c
     * does the former for A8XX through the A8XX register pair, and skips
     * retention entirely, so there is no cx_misc access on this path. */
    if(core_write(d,REG_GMU_RPMH_POWER_STATE,0xf,r) ||
        core_write(d,REG_GMU_CX_FALNEXT_INTF,1,r) ||
        core_write(d,REG_GMU_CX_FAL_INTF,1,r)) {status=ShengGmuIoError;goto failed;}
    /* All shared allocations are real owned coherent memory, not simulated
     * GMU addresses. Zero only after actual stopped/idle checks above. */
    const ShengGmuMemory *memory[]={&c->icache,&c->dummy,&c->debug,&c->log};
    r->memory_changed=1;
    for(unsigned m=0;m<4;++m)for(size_t i=0;i<memory[m]->bytes/4;++i)memory[m]->cpu[i]=0;
    if(AdrenoHfiInitializeStopped(&d->hfi,c->hfi.cpu,c->hfi.bytes,c->hfi.iova,d,notify)!=AdrenoHfiSuccess) {
        status=ShengGmuArgument;goto failed;
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
                if(core_write(d,reg+(uint32_t)(at/4),word,r)) {status=ShengGmuIoError;goto failed;}
            }
        }
    }
    if(d->io.barrier(d->io.cookie)) {status=ShengGmuIoError;goto failed;}
    /* Read every TCM payload word before releasing reset. CPU cache readback
     * checks copied bytes only; actual GMU coherency is still a prerequisite. */
    for(size_t i=0;i<count;++i) {
        const AdrenoFirmwareBlock *b=blocks+i;
        for(size_t at=0;at<b->bytes;at+=4) {
            if(b->region_identifier==2)value=c->icache.cpu[(b->address-0x4000+at)/4];
            else {
                uint32_t reg=b->region_identifier==1?REG_GMU_CM3_ITCM_START+b->address/4:
                    REG_GMU_CM3_DTCM_START+(b->address-0x10004000)/4;
                if(core_read(d,reg+(uint32_t)(at/4),&value,r)) {status=ShengGmuIoError;goto failed;}
            }
            if(value!=le32(c->firmware+b->payload_offset+at)) {status=ShengGmuStateMismatch;goto failed;}
        }
    }
    r->phase=4;
    /* Everything the CPU wrote above has to be in memory before the CM3 is
     * released, because the GMU reads these buffers through its SMMU as Normal
     * non-cacheable. A board whose CPU view is cacheable supplies this hook; the
     * A8XX reference does the same per segment (dc civac then dsb sy). Buffers
     * the board mapped uncached are covered harmlessly, and a board that needs no
     * maintenance leaves the hook null. */
    if(d->services.flush) {
        const ShengGmuMemory *all[]={&c->icache,&c->dummy,&c->debug,&c->log,&c->hfi};
        for(unsigned m=0;m<5;++m)
            if(d->services.flush(d->services.cookie,(const void *)all[m]->cpu,all[m]->bytes)) {
                status=ShengGmuIoError;goto failed;
            }
    }
    const uint32_t fence=UINT32_C(0x80000000)|
        (SHENG_GMU_FENCE_UPPER<<18)|SHENG_GMU_FENCE_LOWER;
    if(core_write(d,REG_GMU_CM3_FW_INIT_RESULT,0,r) ||
        core_write(d,REG_GMU_CM3_BOOT_CONFIG,2,r) ||
        core_write(d,REG_GMU_HFI_QTBL_ADDR,(uint32_t)c->hfi.iova,r) ||
        core_write(d,REG_GMU_HFI_QTBL_INFO,1,r) ||
        core_write(d,REG_GMU_AHB_FENCE_RANGE_0,fence,r) ||
        core_write(d,REG_GMU_CM3_CFG,0x4052,r) ||
        core_write(d,REG_GMU_GENERAL_10,SHENG_GMU_FW_CHIP_ID,r) ||
        core_write(d,REG_GMU_GENERAL_8,((uint32_t)c->log.iova&~UINT32_C(0xfff))|
            ((uint32_t)(c->log.bytes/4096-1)&0xffu),r)) {status=ShengGmuIoError;goto failed;}
    /* a840_gbif[], the only A8XX GBIF list the pinned catalog publishes:
     * side 0..3 first, then CX config, then the GMU MRC QoS word. */
    for(unsigned i=0;i<4;++i)
        if(write32(d,ShengGmuGpu,(REG_GBIF_QSB_SIDE0+i)*4,SHENG_GMU_GBIF_QSB_SIDE_VALUE,r)) {
            status=ShengGmuIoError;goto failed;
        }
    if(write32(d,ShengGmuGpu,REG_GBIF_CX_CONFIG*4,SHENG_GMU_GBIF_CX_CONFIG_VALUE,r) ||
        core_write(d,REG_GMU_MRC_GBIF_QOS_CTRL,SHENG_GMU_MRC_QOS_VALUE,r) ||
        /* The GMU reaches its icache, log and HFI through this interface. */
        write32(d,ShengGmuGpu,REG_GPU_GBIF_HALT*4,SHENG_GMU_GBIF_HALT_RELEASE,r)) {
        status=ShengGmuIoError;goto failed;
    }
    if(core_write(d,REG_GMU_SYS_BUS_CONFIG,1,r) || core_write(d,REG_GMU_ICACHE_CONFIG,1,r) ||
        core_write(d,REG_GMU_DCACHE_CONFIG,1,r) || core_read(d,REG_GMU_CM3_DTCM_START+0xff8,&value,r)) {status=ShengGmuIoError;goto failed;}
    r->dtcm_version=value;
    uint32_t mask=value<=UINT32_C(0x20010004)?UINT32_MAX:0x1ff;
    uint32_t ready=mask==UINT32_MAX?UINT32_C(0xbabeface):0x100;
    if(core_write(d,REG_GMU_CM3_SYSRESET,1,r) || core_write(d,REG_GMU_GENERAL_9,0,r) ||
        d->io.barrier(d->io.cookie)) {status=ShengGmuIoError;goto failed;}
    const uint32_t config_regs[]={REG_GMU_CM3_FW_INIT_RESULT,REG_GMU_CM3_BOOT_CONFIG,
        REG_GMU_HFI_QTBL_ADDR,REG_GMU_HFI_QTBL_INFO,REG_GMU_CM3_CFG,REG_GMU_GENERAL_10,
        REG_GMU_GENERAL_8,REG_GMU_AHB_FENCE_RANGE_0,REG_GMU_SYS_BUS_CONFIG,
        REG_GMU_ICACHE_CONFIG,REG_GMU_DCACHE_CONFIG};
    const uint32_t config_values[]={0,2,(uint32_t)c->hfi.iova,1,0x4052,SHENG_GMU_FW_CHIP_ID,
        ((uint32_t)c->log.iova&~UINT32_C(0xfff))|((uint32_t)(c->log.bytes/4096-1)&0xffu),
        fence,1,1,1};
    for(unsigned i=0;i<sizeof(config_regs)/sizeof(config_regs[0]);++i) {
        if(core_read(d,config_regs[i],&value,r)) {status=ShengGmuIoError;goto failed;}
        if(value!=config_values[i]) {status=ShengGmuStateMismatch;goto failed;}
    }
    r->reset_verified=0;
    if(core_write(d,REG_GMU_CM3_SYSRESET,0,r) || d->io.barrier(d->io.cookie)) {status=ShengGmuIoError;goto failed;}
    r->phase=5;
    uint32_t ao_before=0,ao_after=0;
    (void)core_read(d,REG_GMU_ALWAYS_ON_LO,&ao_before,r);
    status=poll(d,REG_GMU_CM3_FW_INIT_RESULT,mask,ready,c->poll_budget,&r->firmware_init_result,r);
    (void)core_read(d,REG_GMU_ALWAYS_ON_LO,&ao_after,r);
    r->ao_delta=ao_after-ao_before;
    if(status!=ShengGmuSuccess)goto failed;
    r->firmware_ready_observed=1;
    if(core_write(d,REG_GMU_HFI_CTRL_INIT,1,r) || d->io.barrier(d->io.cookie)) {status=ShengGmuIoError;goto failed;}
    r->phase=6;
    status=poll(d,REG_GMU_HFI_CTRL_STATUS,1,1,c->poll_budget,&r->hfi_status,r);
    if(status!=ShengGmuSuccess)goto failed;
    status=check_faults(d,r);if(status!=ShengGmuSuccess)goto failed;
    if(d->io.barrier(d->io.cookie)) {status=ShengGmuIoError;goto failed;}
    r->hfi_ready_observed=1;r->phase=7;return ShengGmuSuccess;
failed:
    reset_after_failure(d,r);return status;
}
typedef struct {
    ShengGmuDevice *device;
    ShengGmuStartupResult *result;
} StartupContext;
static int startup_now(void *cookie,uint64_t *value)
{
    ShengGmuDevice *d=((StartupContext *)cookie)->device;
    return d->services.now_us(d->services.cookie,value);
}
static int startup_wait(void *cookie,unsigned us)
{
    ShengGmuDevice *d=((StartupContext *)cookie)->device;
    return d->services.wait_us(d->services.cookie,us);
}
static int startup_health(void *cookie)
{
    StartupContext *c=cookie;ShengGmuDevice *d=c->device;
    ShengGmuStartupResult *r=c->result;uint32_t reset,status;
    r->health_status=check_faults(d,&r->load);
    if(r->health_status!=ShengGmuSuccess)return 1;
    if(core_read(d,REG_GMU_CM3_SYSRESET,&reset,&r->load) ||
        core_read(d,REG_GMU_HFI_CTRL_STATUS,&status,&r->load))r->health_status=ShengGmuIoError;
    else if(reset!=0 || !(status&1))r->health_status=ShengGmuStateMismatch;
    return r->health_status!=ShengGmuSuccess;
}
ShengGmuStatus ShengGmuColdStart(ShengGmuDevice *d,const ShengGmuConfig *c,
    const AdrenoHfiStartupConfig *config,const ShengGmuStartupLimits *limits,ShengGmuStartupResult *r)
{
    if(!r)return ShengGmuArgument;
    *r=(ShengGmuStartupResult){0};r->load.resources_must_retain=r->protocol.resources_must_retain=1;
    r->protocol_status=AdrenoHfiStartupInvalid; /* Not attempted until the load succeeds. */
    if(!d || !c || !limits || AdrenoHfiStartupValidate(config)!=AdrenoHfiStartupSuccess)return ShengGmuArgument;
    StartupContext context={d,r};
    AdrenoHfiStartupServices services={&context,startup_now,startup_wait,startup_health,
        limits->polls_per_command,limits->wait_interval_us,limits->command_timeout_us,limits->total_timeout_us};
    if(AdrenoHfiStartupValidateServices(&services)!=AdrenoHfiStartupSuccess)return ShengGmuArgument;
    if(d->poisoned || d->startup.poisoned)return ShengGmuPoisoned;
    if(d->startup.attempted || d->startup.completed)return ShengGmuBusy;
    ShengGmuStatus status=ShengGmuLoadAndStartHfi(d,c,&r->load);
    if(status!=ShengGmuSuccess)return status;
    r->protocol_status=AdrenoHfiStartupRun(&d->startup,&d->hfi,config,&services,&r->protocol);
    if(r->protocol_status==AdrenoHfiStartupSuccess)return ShengGmuSuccess;
    switch(r->protocol_status) {
    case AdrenoHfiStartupClockError:case AdrenoHfiStartupWaitError:status=ShengGmuClockError;break;
    case AdrenoHfiStartupTimeout:status=ShengGmuTimeout;break;
    case AdrenoHfiStartupPlatformFault:status=r->health_status;break;
    default:status=ShengGmuProtocolError;break;
    }
    reset_after_failure(d,&r->load);d->startup.poisoned=1;
    return status;
}
static int fits(ShengGmuMmio *m,unsigned window,size_t at)
{
    if(!m || window>=ShengGmuWindows)return 0;
    ShengGmuMmioWindow *w=m->windows+window;
    return extent((uintptr_t)w->cpu,w->bytes) && !((uintptr_t)w->cpu&3) && !(at&3) &&
        at<=w->bytes && w->bytes-at>=4;
}
static int mmio_read(void *cookie,unsigned window,size_t at,uint32_t *v)
{ShengGmuMmio *m=cookie;if(!v || !fits(m,window,at))return 1;*v=*(volatile uint32_t *)(m->windows[window].cpu+at);return 0;}
static int mmio_write(void *cookie,unsigned window,size_t at,uint32_t v)
{ShengGmuMmio *m=cookie;if(!fits(m,window,at))return 1;*(volatile uint32_t *)(m->windows[window].cpu+at)=v;return 0;}
static int mmio_barrier(void *cookie)
{(void)cookie;__asm__ volatile("dsb sy":::"memory");return 0;}
ShengGmuStatus ShengGmuBindMmio(ShengGmuMmio *m,ShengGmuIo *out)
{
    if(!out)return ShengGmuArgument;
    *out=(ShengGmuIo){0};
    if(!m)return ShengGmuArgument;
    for(unsigned i=0;i<ShengGmuWindows;++i) {
        const ShengGmuMmioWindow *w=m->windows+i;
#if !SHENG_GMU_PDC_PRESENT
        /* An absent PDC window is exactly "no mapping, no size". */
        if(i==ShengGmuPdc) {
            if(w->physical || w->bytes)return ShengGmuArgument;
            out->bytes[i]=0;
            continue;
        }
#endif
        if(!fits(m,i,0) || w->physical!=sheng_gmu_window_physical[i] ||
            w->bytes!=sheng_gmu_window_bytes[i])return ShengGmuArgument;
        for(unsigned j=0;j<i;++j)if(overlap((uintptr_t)w->cpu,w->bytes,(uintptr_t)m->windows[j].cpu,m->windows[j].bytes))return ShengGmuArgument;
        out->bytes[i]=w->bytes;
    }
    out->cookie=m;out->read32=mmio_read;out->write32=mmio_write;out->barrier=mmio_barrier;
    return ShengGmuSuccess;
}

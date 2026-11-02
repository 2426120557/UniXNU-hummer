/* SPDX-License-Identifier: MIT */
/* hummer A830 board connection. Called once, after synchronous peripheral
 * startup. Every exposed allocation and hardware vote lives until reboot.
 *
 * Adopted from IssacL04/UniXNU-Pagani_SM8750 (OnePlus 13T PKX110): same SM8750,
 * same Adreno 830, and that port reaches a Metal desktop with this exact chain.
 * What changed for hummer:
 *
 *   - the votes/OPP policy is HummerGpuBuildStartup (package/HummerGpuVotes.c),
 *     and an unrecognised QFPROM speed bin falls back through the four bins the
 *     tables were generated from instead of refusing to start;
 *   - the SoC id is not checked against the 13T's values (0x26a / 0x27f): hummer
 *     is a different SM8750 SKU and its id is only reported, not required;
 *   - progress goes through boot_progress_display_status(), which this board
 *     implements against its own panel renderer (package/Stage7Status.cpp);
 *   - the display-side frame counters the 13T port prints (native_gop_gpu_*) do
 *     not exist here, so the status readout is just the stage and return code.
 *
 * The chain is: board inputs -> GPUCC power -> SMMU -> firmware memory -> GMU
 * cold start + HFI -> CP init -> ZAP -> a PM4 scratch write proving the ring
 * works -> publish -> the Metal accelerator. Every step reports itself before it
 * runs, so a hang leaves its own name on the panel. */
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <mach/task.h>
#include <pexpert/pexpert.h>
#include "ArmSmmu4KMmio.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoA830HwInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoDiagnostics.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoSha256.h"
#include "AdrenoPM4.h"
#include "Sm8750Gmu.h"
#include "Sm8750GmuSource.h"
#include "Sm8750GpuFirmware.h"
#include "Sm8750GpuPower.h"
#include "Sm8750GpuSmmu.h"
#include "Sm8750GpuZap.h"
#include "Sm8750SocInfo.h"
#include "HummerGpuVotes.h"

/* The board's own panel renderer; implemented in package/Stage7Status.cpp. */
extern "C" void boot_progress_display_status(const char *detail, uint64_t value);
extern "C" void native_adreno_set_gpu_context(NativeAdrenoGpuContext *);
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *,NativeAdrenoSubmit *,NativeAdrenoRing *,
    const NativeAdrenoRingClock *,const NativeAdrenoTimer *,const NativeAdrenoAbiDeviceInfo *,uint32_t,uint32_t);
extern "C" IOService *native_adreno_published_service();
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" volatile uint32_t native_adreno_wc_enabled;
/* Both are owned here because this board starts the GPU itself: the engine's
 * NativeAdrenoService.cpp (the other board entry) defines them too, and it is
 * not part of this archive -- see scripts/build-hummer-adreno-kits.py. */
extern "C" {
volatile uint32_t native_gpu_dcvs[15];
volatile uint64_t native_adreno_fence_phys=0;
}
/* The accelerator lives in its own staged archive (scripts/build-native-adreno-
 * accelerator.py): its static initializer must run only after the original
 * IOGPUFamily kext is loaded, so it is exported under its own name instead of
 * being registered in __mod_init_func. Both this file and that staging step may
 * be absent when only the kernel half is built, hence the weak references. */
extern "C" void native_adreno_accelerator_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp") __attribute__((weak));
extern "C" IOService *native_adreno_accelerator_allocate(void) __attribute__((weak));
extern "C" unsigned native_adreno_accelerator_instance_size(void) __attribute__((weak));

/* NativeAdrenoPublish.cpp (the AGXDeviceUserClient behind the Metal service)
 * imports the display driver's UEFI-GOP handoff: the framebuffer the bootloader
 * left behind, handed to the GPU so the first scanout is a render rather than a
 * copy. hummer has no such provider (its panel path is the early-console
 * renderer, and the legacy display driver owns scanout), so the handoff is
 * refused here. That costs nothing on this board: attach still publishes the
 * service, and nothing asks for the handoff unless a client does.
 *
 * boot_log_export_put is the boot-progress log export; this board does not build
 * that driver either (package/Stage7Status.cpp explains why), so the kernel log
 * stays in the kernel. All four are declared weak_import in the driver, which a
 * statically linked kernel refuses -- they have to be defined. */
extern "C" IOReturn native_gop_handoff_call(uint32_t, task_t, const void *, size_t,
                                            void *, size_t) {return kIOReturnUnsupported;}
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void) {return nullptr;}
extern "C" void native_gop_handoff_close(task_t) {}
extern "C" int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) {return -1;}

namespace {
struct Window {
    OSSharedPtr<IOMemoryMap> map;
    volatile uint8_t *cpu=nullptr;
    size_t bytes=0;
    bool open(uint64_t physical,size_t length,bool memory=false) {
        /* An IOMemoryDescriptor over the physical range, mapped cache-inhibited
         * (or write-combined for the buffers the GPU writes back). */
        IODeviceMemory *descriptor=IODeviceMemory::withRange(physical,length);
        if(!descriptor)return false;
        map=OSSharedPtr<IOMemoryMap>(descriptor->map(kIOMapAnywhere |
            (memory?kIOMapWriteCombineCache:kIOMapInhibitCache)),OSNoRetain);
        descriptor->release();
        if(!map)return false;
        cpu=reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());bytes=length;return cpu;
    }
    bool fits(size_t at) const {return cpu && !(at&3) && at<=bytes && bytes-at>=4;}
    int read(size_t at,uint32_t *value) const {
        if(!value || !fits(at))return -1;
        *value=*reinterpret_cast<volatile uint32_t *>(cpu+at);
        __asm__ volatile("dsb sy":::"memory");return 0;
    }
    int write(size_t at,uint32_t value) {
        if(!fits(at))return -1;
        *reinterpret_cast<volatile uint32_t *>(cpu+at)=value;
        __asm__ volatile("dsb sy":::"memory");return 0;
    }
};
struct State {
    bool attempted=false,irq_ready=false,faulted=false;
    Window gcc,gpucc,smmu,rsc,cmddb,smem,qfprom,cxmisc,zap,core[Sm8750GmuWindows];
    Window gmu_memory[5],gpu_memory[GpuMaxBufferCount];
    NativeAdrenoMemory db_snapshot,zap_metadata;
    Sm8750GmuVoteInputs votes{};
    AdrenoHfiStartupConfig startup{};
    Sm8750GpuPowerResult power{};
    Sm8750GmuMmio mmio{};
    Sm8750GmuDevice gmu{};
    Sm8750GmuConfig config{};
    Sm8750GmuStartupResult gmu_result{};
    Sm8750GmuResult runtime{};
    Sm8750GpuZapResult zap_result{};
    Sm8750ScmResult cx_result{};
    ArmSmmu4KMmio smmu_mmio{};
    ArmSmmu4KIo smmu_io{};
    Sm8750GpuSmmuResult smmu_plan{};
    NativeAdrenoGmuContext gmu_context;
    NativeAdrenoGpuContext gpu_context;
    NativeAdrenoGmuContextReport gmu_report{},gpu_report{};
    NativeAdrenoRing ring;
    NativeAdrenoCpInit cp;
    NativeAdrenoA830HwInit hardware;
    NativeAdrenoIrq irq;
    NativeAdrenoSubmit submit;
    NativeAdrenoTimer timer;
    size_t bank_offset[2]{};
    unsigned smmu_context=0;
    unsigned initial=0,bandwidth=0;
    uint32_t soc_id=0,soc_code=0,speed_bin=0,speed_used=0,speed_fallback=0;
    uint64_t requested_hz=0;
} g;
int barrier(void *) {__asm__ volatile("dsb sy":::"memory");return 0;}
int now(void *,uint64_t *value) {
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);*value=ns/1000;return 0;
}
uint64_t ring_now(void *) {uint64_t value;now(nullptr,&value);return value;}
int delay(void *,unsigned us) {if(us>1000000)return -1;IODelay(us);return 0;}
int ring_idle(void *) {IODelay(10);return 0;}
const char *current_stage="GPU start";
int startup_result=kIOReturnNotReady;
void stage(const char *name,uint64_t value=0) {
    __atomic_store_n(&current_stage,name,__ATOMIC_RELEASE);boot_progress_display_status(name,value);
    IOLog("hummer GPU: %s %llx\n",name,static_cast<unsigned long long>(value));
}
int hash(void *,const void *bytes,size_t count,uint8_t digest[32]) {
    NativeAdrenoSha256::hash(static_cast<const uint8_t *>(bytes),count,digest);return 0;
}
uint32_t rsc_read(void *,uint32_t at) {uint32_t value=~0u;g.rsc.read(at,&value);return value;}
void rsc_write(void *,uint32_t at,uint32_t value) {(void)g.rsc.write(at,value);}
void rsc_delay(void *,unsigned us) {IODelay(us);}
int vote(const rpmh_command *commands,unsigned count) {
    const peripheral_io io={nullptr,rsc_read,rsc_write,rsc_delay};rpmh_result result{};
    int rc=sm8750_rpmh_write(&io,commands,count,&result);
    if(rc)stage("GPU RPMh failed",(uint64_t(result.tcs)<<32)|uint32_t(rc));
    return rc;
}
uint32_t maximum(const AdrenoHfiRail &rail) {
    uint32_t value=0;
    for(size_t i=0;i+1<rail.bytes;i+=2) {
        uint32_t v=uint32_t(rail.data[i])|uint32_t(rail.data[i+1])<<8;
        if(v>value)value=v;
    }
    return value;
}
int rails(void *,uint32_t cx,uint32_t mx) {
    // shortcut: retain maximum valid shared corners; replace when inherited display/CPU votes have a common owner.
    uint32_t cx_max=maximum(g.votes.cx),mx_max=maximum(g.votes.mx);
    if(cx_max<cx || mx_max<mx)return -1;
    rpmh_command commands[2];
    return Sm8750GpuRailCommands(&g.votes,cx_max,mx_max,commands) || vote(commands,2);
}
int power_read(void *,unsigned window,uint32_t at,uint32_t *value) {
    return window>1?-1:(window?g.gpucc:g.gcc).read(at,value);
}
int power_write(void *,unsigned window,uint32_t at,uint32_t value) {
    return window>1?-1:(window?g.gpucc:g.gcc).write(at,value);
}
int gpu_read(void *,uint32_t reg,uint32_t *value) {return g.core[Sm8750GmuGpu].read(size_t(reg)*4,value);}
int gpu_write(void *,uint32_t reg,uint32_t value) {return g.core[Sm8750GmuGpu].write(size_t(reg)*4,value);}
int gmu_read(void *,uint32_t reg,uint32_t *value) {
    if(reg<SM8750_GMU_CORE_GPU_DELTA/4)return -1;
    return g.core[Sm8750GmuCore].read(size_t(reg)*4-SM8750_GMU_CORE_GPU_DELTA,value);
}
int gmu_write(void *,uint32_t reg,uint32_t value) {
    if(reg<SM8750_GMU_CORE_GPU_DELTA/4)return -1;
    return g.core[Sm8750GmuCore].write(size_t(reg)*4-SM8750_GMU_CORE_GPU_DELTA,value);
}
int fence_status(void *,int clear,uint32_t *value) {
    return clear?gmu_write(nullptr,0x23b14,7):gmu_read(nullptr,0x23b13,value);
}
const NativeAdrenoRingIo gpu_io={nullptr,gpu_write,gpu_read,barrier,nullptr,fence_status};
const NativeAdrenoRingIo gmu_io={nullptr,gmu_write,gmu_read,barrier,nullptr,nullptr};
const NativeAdrenoRingClock clock_io={nullptr,ring_now,ring_idle};
bool range(const Window &w,const volatile void *p,size_t bytes) {
    uintptr_t at=reinterpret_cast<uintptr_t>(p),base=reinterpret_cast<uintptr_t>(w.cpu);
    return w.cpu && at>=base && at-base<=w.bytes && bytes<=w.bytes-(at-base);
}
int health() {
    if(g.faulted)return -1;
    uint32_t value;
    if(gmu_read(nullptr,REG_GMU_AO_HOST_INTERRUPT_STATUS,&value) || (value&SM8750_GMU_AO_FATAL_MASK) ||
        gmu_read(nullptr,REG_GMU_GMU2HOST_INTR_INFO,&value) || (value&SM8750_GMU_CM3_FATAL_MASK) ||
        g.smmu.read(0x48,&value) || value)g.faulted=true;
    for(unsigned i=0;i<2 && !g.faulted;++i)
        if(g.smmu.read(g.bank_offset[i]+0x58,&value) || (value&0xc00001fe))g.faulted=true;
    if(g.irq_ready && !g.faulted) {
        NativeAdrenoIrqResult irq{};
        if(g.irq.handle(&irq) || (irq.flags&NativeAdrenoIrqFatal)) {
            stage("GPU IRQ fault",(uint64_t(irq.status)<<32)|irq.cp_status);g.faulted=true;
        }
    }
    return g.faulted?-1:0;
}
int publish(void *,const volatile uint32_t *p,size_t bytes) {
    for(const auto &w:g.gpu_memory)if(range(w,p,bytes))return barrier(nullptr) || health();
    return -1;
}
int acquire(void *,volatile uint32_t *p,size_t bytes) {
    return !range(g.gpu_memory[2],p,bytes) || barrier(nullptr) || health();
}
const NativeAdrenoRingCache visibility={nullptr,publish,acquire};
int scm_publish(void *,const void *p,size_t bytes) {
    if(p==g.zap_metadata.mapping() && bytes==g.zap_metadata.bytes())
        return g.zap_metadata.publishForDevice(0,bytes,true);
    return range(g.zap,p,bytes)?barrier(nullptr):-1;
}
const Sm8750ScmIo scm_io={nullptr,Sm8750ScmInvoke,now,delay,scm_publish};
IOReturn authenticate(void *) {
    if(!sm8750_gpu_zap_metadata_size || !sm8750_gpu_zap_payload_size) {
        /* No vendor ZAP image staged: say so once, and let the CP path fail
         * here rather than after a second of SCM retries. */
        stage("GPU ZAP image missing",0);
        return kIOReturnUnsupported;
    }
    if(Sm8750GpuZapLoad(&scm_io,sm8750_gpu_zap_metadata,size_t(sm8750_gpu_zap_metadata_size),
        sm8750_gpu_zap_payload,size_t(sm8750_gpu_zap_payload_size),g.zap_metadata.mapping(),
        g.zap_metadata.spans()[0].physical,g.zap.cpu,&g.zap_result))return kIOReturnIOError;
    return kIOReturnSuccess;
}
int snapshot(Window &window,NativeAdrenoMemory &memory) {
    if(memory.allocate(window.bytes,40,false,window.bytes))return -1;
    auto out=static_cast<uint8_t *>(memory.mapping());
    for(size_t i=0;i<window.bytes;++i)out[i]=window.cpu[i];
    return barrier(nullptr);
}
int prepare_inputs() {
    if(!g.cmddb.open(0x81c60000,0x20000,true) || !g.smem.open(0x81d00000,0x200000,true) ||
        !g.qfprom.open(0x221c8000,0x1000) || snapshot(g.cmddb,g.db_snapshot))return -1;
    // Other SMEM partitions can abort reads; visit only global socinfo metadata and fields.
    if(Sm8750GmuCommandDbInputs(static_cast<const uint8_t *>(g.db_snapshot.mapping()),g.db_snapshot.bytes(),&g.votes) ||
        Sm8750SocInfoGpuCode(g.smem.cpu,g.smem.bytes,&g.soc_id,&g.soc_code))return -1;
    // The 13T port requires its own SoC ids here. hummer is a different SM8750
    // SKU, so instead of refusing an id nobody has read on this board yet, the
    // id must only be stable across two reads (a torn bootloader write would
    // otherwise pick an OPP table from garbage).
    uint32_t second_id,second_code;
    barrier(nullptr);
    if(Sm8750SocInfoGpuCode(g.smem.cpu,g.smem.bytes,&second_id,&second_code) ||
        second_id!=g.soc_id || second_code!=g.soc_code || g.qfprom.read(0x138,&g.speed_bin))return -1;
    g.speed_bin&=511;
    g.speed_used=g.speed_bin;
    if(HummerGpuBuildStartup(&g.votes,g.speed_bin,g.soc_code,&g.startup,&g.initial,&g.bandwidth)) {
        // The tables were generated from four 9-bit bins; when the fuse reads
        // something else, try them in turn. The top OPPs differ per bin, but
        // every profile starts at 160 MHz and this bring-up asks for <=525 MHz.
        static const uint32_t bins[]={190,221,232,242};
        int rc=1;
        for(unsigned i=0;i<sizeof(bins)/sizeof(bins[0]);++i) {
            rc=HummerGpuBuildStartup(&g.votes,bins[i],g.soc_code,&g.startup,&g.initial,&g.bandwidth);
            if(!rc) {g.speed_used=bins[i];g.speed_fallback=1;break;}
        }
        if(rc)return -1;
    }
    return 0;
}
int prepare_smmu() {
    stage("GPU SMMU bind");
    g.smmu_mmio={g.smmu.cpu,g.smmu.bytes};
    int rc=ArmSmmu4KBindMmio(&g.smmu_mmio,&g.smmu_io);if(rc)return rc;
    stage("GPU SMMU select");
    rc=Sm8750GpuSmmuPrepare(&g.smmu_io,&g.smmu_plan);
    if(rc)return rc;
    for(unsigned i=0;i<2;++i)g.bank_offset[i]=g.smmu_plan.bank_offset[i];
    stage("GPU SMMU ACTLR");
    for(unsigned i=0;i<2;++i) {
        uint32_t value=~0u;
        if(g.smmu.write(g.bank_offset[i]+4,0x32b) || g.smmu.read(g.bank_offset[i]+4,&value) || value!=0x32b)return -5;
    }
    stage("GPU SMMU GMU");g.smmu_context=1;
    rc=g.gmu_context.bringUp(&g.smmu_io,g.smmu_plan.banks[1],1,g.smmu_plan.slots[2],5,nullptr,&g.gmu_report);if(rc)return rc;
    const uint16_t sids[]={0,1};
    native_adreno_wc_enabled=1;
    stage("GPU SMMU GPU");g.smmu_context=2;
    return g.gpu_context.bringUp(&g.smmu_io,0,2,g.smmu_plan.slots,sids,nullptr,&g.gpu_report,NativeAdrenoChipA830);
}
int prepare_memory() {
    auto &a=g.gmu_context.allocator();const auto &l=a.layout();
    const NativeAdrenoMemory *objects[]={&a.icache(),&a.dummy(),&a.debug(),&a.log(),&a.hfi()};
    const uint64_t iovas[]={l.icache_iova,l.dummy_iova,l.debug_iova,l.log_iova,l.hfi_iova};
    Sm8750GmuMemory *memory[]={&g.config.icache,&g.config.dummy,&g.config.debug,&g.config.log,&g.config.hfi};
    for(unsigned i=0;i<5;++i) {
        const auto &m=*objects[i];
        if(m.spanCount()!=1 || !g.gmu_memory[i].open(m.spans()[0].physical,m.bytes(),true))return -1;
        *memory[i]={reinterpret_cast<volatile uint32_t *>(g.gmu_memory[i].cpu),m.bytes(),iovas[i]};
    }
    for(unsigned i=0;i<g.gpu_context.bufferCount();++i) {
        const auto &m=g.gpu_context.buffer(i);
        if(m.spanCount()!=1 || !g.gpu_memory[i].open(m.spans()[0].physical,m.bytes(),true))return -1;
    }
    if(g.zap_metadata.allocate(0x4000,40,true,0x4000) || g.zap_metadata.spanCount()!=1 ||
        !g.zap.open(0x9b09a000,0x2000,true))return -1;
    const uint8_t *images[]={sm8750_gpu_sqe,sm8750_gpu_aqe};
    const size_t sizes[]={size_t(sm8750_gpu_sqe_size),size_t(sm8750_gpu_aqe_size)};
    const unsigned indices[]={0,5};
    for(unsigned image=0;image<2;++image) {
        // The vendor image carries a four-byte header the CP does not load.
        if(!sizes[image]) {if(image)stage("GPU AQE image missing",0);continue;}
        auto &w=g.gpu_memory[indices[image]];
        if(sizes[image]<8 || sizes[image]-4>w.bytes)return -1;
        for(size_t j=4;j<sizes[image];++j)w.cpu[j-4]=images[image][j];
        barrier(nullptr);
        for(size_t j=4;j<sizes[image];++j)if(w.cpu[j-4]!=images[image][j])return -1;
    }
    native_adreno_fence_phys=g.gpu_context.buffer(2).spans()[0].physical;
    return 0;
}
int metal() {
    if(!native_adreno_accelerator_allocate || !native_adreno_accelerator_instance_size ||
       !native_adreno_accelerator_static_init)return kIOReturnUnsupported;
    int rc=OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");if(rc)return rc;
    const OSSymbol *symbol=OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent=symbol?OSMetaClass::getMetaClassWithName(symbol):nullptr;
    OSSafeReleaseNULL(symbol);
    if(!parent || parent->getClassSize()!=136)return -1;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return -1;
    native_adreno_accelerator_static_init();
    if(OSMetaClass::postModLoad(handle) || native_adreno_accelerator_instance_size()!=136)return -1;
    IOService *service=native_adreno_accelerator_allocate();
    if(!service || !service->init())return -1;
    service->setProperty("MetalPluginName","NativeAdrenoMetal");
    service->setProperty("MetalPluginClassName","NativeAdrenoMetalDevice");
    service->setProperty("NativeSandboxGrantProcess","backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard");
    if(!service->attach(IOService::getPlatform()))return -1;
    service->registerService();return 0;
}
int start() {
    stage("GPU board inputs");if(prepare_inputs())return -1;
    stage("GPU power",(uint64_t(g.speed_bin)<<32)|g.soc_code);
    if(!g.gcc.open(0x100000,0x1f5000) || !g.gpucc.open(0x3d90000,0xa000) ||
        !g.rsc.open(0x16520000,0x10000))return -1;
    const Sm8750GpuPowerIo power={nullptr,power_read,power_write,delay,rails};
    int power_rc=Sm8750GpuPowerOn(&power,&g.power);
    if(power_rc)return power_rc;
    for(unsigned i=0;i<Sm8750GmuWindows;++i) {
        if(!g.core[i].open(sm8750_gmu_window_physical[i],sm8750_gmu_window_bytes[i]))return -1;
        g.mmio.windows[i]={g.core[i].cpu,sm8750_gmu_window_physical[i],g.core[i].bytes};
    }
    if(!g.smmu.open(0x3da0000,0x40000) || !g.cxmisc.open(0x3d9e000,0x2000) ||
        Sm8750GmuBindMmio(&g.mmio,&g.gmu.io))return -1;
    uint32_t reset;
    if(gmu_read(nullptr,REG_GMU_CM3_SYSRESET,&reset) || reset!=1)return -1;
    stage("GPU secure CX");if(Sm8750GpuSecureCxInit(&scm_io,&g.cx_result))return -1;
    rpmh_command cnoc={g.startup.cnoc_addresses[0],g.startup.cnoc_data[1][0]},ddr[3];
    if(g.startup.cnoc_count!=1 || g.startup.ddr_count!=3 || g.bandwidth>=g.startup.bandwidth_count)return -1;
    // Retain the top HLOS bus vote while inherited display consumers have no shared bandwidth owner.
    for(unsigned i=0;i<3;++i)ddr[i]={g.startup.ddr_addresses[i],g.startup.ddr_data[g.startup.bandwidth_count-1][i]};
    if(vote(&cnoc,1) || vote(ddr,3))return -1;
    stage("GPU SMMU");int smmu_rc=prepare_smmu();if(smmu_rc)return smmu_rc;
    stage("GPU firmware memory");if(prepare_memory())return -1;
    g.gmu.services={nullptr,hash,now,delay};
    g.config.chip_id=NativeAdrenoChipA830;g.config.firmware=sm8750_gpu_gmu;
    g.config.firmware_bytes=size_t(sm8750_gpu_gmu_size);g.config.poll_budget=1000;
    // Poll status and fences; leave unowned GIC sources masked at the device.
    if(gmu_write(nullptr,0x23b06,~0u) || gmu_write(nullptr,0x1f993,~0u))return -1;
    const Sm8750GmuStartupLimits limits={1000,100,100000,2000000};
    stage("GPU GMU HFI");
    if(Sm8750GmuColdStart(&g.gmu,&g.config,&g.startup,&limits,&g.gmu_result) ||
        Sm8750GmuSetPerformance(&g.gmu,g.initial,g.bandwidth,&g.runtime))return -1;
    g.requested_hz=g.startup.gpu[g.initial].hz;
    native_gpu_dcvs[1]=g.initial;
    if(Sm8750GmuGpuOob(&g.gmu,1,&g.runtime))return -1;
    uint32_t gx;
    if(gmu_read(nullptr,0x1f7e8,&gx) || (gx&3) || g.cxmisc.read(0x83*4,&gx) || (gx&0x18))return -1;
    stage("GPU command processor");
    if(gpu_write(nullptr,0x62,0) || g.irq.attach(&gpu_io,NativeAdrenoChipA830))return -1;
    g.irq_ready=true;
    if(g.ring.attach(&gpu_io,&visibility,const_cast<uint8_t *>(g.gpu_memory[1].cpu),GpuRingBytes,GpuRingIova,
        const_cast<uint8_t *>(g.gpu_memory[2].cpu),GpuPageBytes,GpuMemptrsIova,NativeAdrenoChipA830) ||
        g.hardware.configure(&gpu_io,&gmu_io,16,GpuSqeIova,GpuAqeIova))return -1;
    const NativeAdrenoCpInitConfig cp={GpuSqeIova,GpuPwrupIova,1,GpuAqeIova};uint32_t fence,observed;
    if(g.cp.run(&gpu_io,&g.ring,&cp,&fence) || g.ring.wait(fence,&clock_io,200000,&observed)!=NativeAdrenoRingFenceComplete)return -1;
    stage("GPU ZAP secure exit");
    if(g.hardware.finish(&g.ring,&g.cp,&clock_io,200000,authenticate,nullptr,
        const_cast<uint8_t *>(g.gpu_memory[3].cpu),GpuPageBytes,&visibility))return -1;
    stage("GPU memory fence");
    auto scratch=reinterpret_cast<volatile uint32_t *>(g.gpu_memory[4].cpu);scratch[0]=0;barrier(nullptr);
    uint32_t packet[4];if(AdrenoPM4Type7(0x3d,3,packet))return -1;
    packet[1]=uint32_t(GpuScratchIova);packet[2]=0;packet[3]=0x8750830;
    if(g.ring.submit(packet,4,&fence) || g.ring.wait(fence,&clock_io,200000,&observed)!=NativeAdrenoRingFenceComplete ||
        scratch[0]!=packet[3] || health())return -1;
    static const NativeAdrenoAbiDeviceInfo info={NativeAdrenoChipA830,12u<<20,0x4000000,0x1fffffffff000ull,
        0x20000000,0x60000000,16,6,1,5,NativeAdrenoDeviceClientIova,0};
    const NativeAdrenoTimeIo time={nullptr,gpu_read};
    native_adreno_set_gpu_context(&g.gpu_context);
    if(g.submit.attach(g.gpu_context.vm(),&g.ring) || g.timer.attach(&time,NativeAdrenoTimeCp,NativeAdrenoChipA830) ||
        native_adreno_publish(g.gpu_context.vm(),&g.submit,&g.ring,&clock_io,&g.timer,&info,g.gpu_context.inputBits(),g.gpu_report.output_bits))return -1;
    stage("GPU Metal service");return metal();
}
}
extern "C" int hummer_gpu_start() {
    if(g.attempted)return kIOReturnBusy;
    /* Opt-in, exactly as the register probes were: an unset argument must leave
     * the machine exactly as it boots today. */
    uint32_t enabled=0;
    if(!PE_parse_boot_argn("hummer-adreno-bringup",&enabled,sizeof(enabled)) || !enabled)
        return kIOReturnUnsupported;
    g.attempted=true;
    int rc=start();
    if(rc) {boot_progress_display_status(current_stage,uint32_t(rc));IOLog("hummer GPU stopped at %s: %d\n",current_stage,rc);}
    else stage("GPU Metal registered",NativeAdrenoChipA830);
    __atomic_store_n(&startup_result,rc,__ATOMIC_RELEASE);
    return rc;
}
/* One line for the board's own status surface: where it stopped, why, and --
 * when it did not stop -- the clock it asked for and whether the GMU reports
 * the GPU busy. The 13T port prints six more lines of frame counters here; this
 * board has no display-side GPU handoff yet, so there is nothing to print. */
extern "C" int hummer_gpu_status(char *line,unsigned bytes) {
    const char *name=__atomic_load_n(&current_stage,__ATOMIC_ACQUIRE);
    const int rc=__atomic_load_n(&startup_result,__ATOMIC_ACQUIRE);
    if(!line || !bytes)return -1;
    uint32_t busy=~0u;
    char activity='-';
    if(!rc && g.gmu.gpu_oob_held && !g.gmu.poisoned)
        activity=gmu_read(nullptr,0x23b0c,&busy)?'?':((busy&(1u<<23))?'1':'0');
    snprintf(line,bytes,"%s rc=%08x MHz=%u busy=%c",name,uint32_t(rc),
        unsigned(g.requested_hz/1000000),activity);
    return rc;
}

// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalBacklightABI.h"
#include "../PaganiInput/Mapping.h"
#include <PaganiPeripherals.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSNumber.h>
#include "../BootProgress/PeripheralStatus.h"

// The display PM callback publishes a value, never a pointer into a detachable service.
static unsigned requestedPower=1;
extern "C" void pagani_panel_set_power(unsigned on) {__atomic_store_n(&requestedPower,on?1:0,__ATOMIC_RELEASE);}

class PaganiPanel : public AppleARMBacklight {
    OSDeclareDefaultStructors(PaganiPanel)
    PeripheralWindow dsi;
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    bool added=false,running=false,failed=false;
    unsigned appliedPower=1,level=PAGANI_BRIGHTNESS_DEFAULT;
    uint32_t requestedLevel=PAGANI_BRIGHTNESS_DEFAULT;
    bool first=true;
    uint64_t commands=0;
    void observe(const char *stage,int rc,uint32_t value) {
        boot_progress_peripheral(PAGANI_PANEL,stage,rc,value,__atomic_load_n(&commands,__ATOMIC_RELAXED));
    }
    void publish() {
        auto b=OSDictionary::withCapacity(3),p=OSDictionary::withCapacity(1);
        auto lo=OSNumber::withNumber(0ULL,32),hi=OSNumber::withNumber(PAGANI_BRIGHTNESS_MAX,32),v=OSNumber::withNumber(level,32);
        if (b && p && lo && hi && v) {
            b->setObject("min",lo);b->setObject("max",hi);b->setObject("value",v);
            p->setObject("brightness",b);setProperty("IODisplayParameters",p);
        }
        OSSafeReleaseNULL(lo);OSSafeReleaseNULL(hi);OSSafeReleaseNULL(v);OSSafeReleaseNULL(b);OSSafeReleaseNULL(p);
        setProperty("brightness",level,32);setProperty("PanelDisplayOn",appliedPower!=0);
    }
    static void tick(OSObject *owner,IOTimerEventSource *) {
        auto self=static_cast<PaganiPanel *>(owner);
        if (!__atomic_load_n(&self->running,__ATOMIC_ACQUIRE) || __atomic_load_n(&self->failed,__ATOMIC_ACQUIRE)) return;
        const unsigned power=__atomic_load_n(&requestedPower,__ATOMIC_ACQUIRE);
        const unsigned goal=__atomic_load_n(&self->requestedLevel,__ATOMIC_ACQUIRE);
        auto io=self->dsi.io();int rc=0;bool changed=false;
        if (power!=self->appliedPower) {
            rc=dsi_panel_command(&io,power?0x29:0x28,0);
            if (!rc) {self->appliedPower=power;changed=true;__atomic_add_fetch(&self->commands,1,__ATOMIC_RELAXED);}
            self->observe("DSI power",rc,power);
        }
        if (!rc && (self->first || (power && goal!=self->level))) {
            rc=dsi_panel_command(&io,0x51,goal);
            if (!rc) {self->level=goal;self->first=false;changed=true;__atomic_add_fetch(&self->commands,1,__ATOMIC_RELAXED);}
            self->observe("DSI brightness",rc,goal);
        }
        if (rc) {
            __atomic_store_n(&self->failed,true,__ATOMIC_RELEASE);self->setProperty("PanelError",(uint32_t)rc,32);
            IOLog("Pagani panel command failed: %d; requests disabled\n",rc);return;
        }
        if (changed) self->publish();
        self->timer->setTimeoutMS(16);
    }
    void cleanup() {
        __atomic_store_n(&running,false,__ATOMIC_RELEASE);
        if (timer) {timer->cancelTimeout();if (added) loop->removeEventSource(timer);}
        added=false;OSSafeReleaseNULL(timer);OSSafeReleaseNULL(loop);dsi.release();
    }
public:
    // The original constructor supplies storage and ancestry; no Apple hardware lifecycle is called.
    bool init(OSDictionary *properties=nullptr) override {return IOService::init(properties);}
    IOWorkLoop *getWorkLoop() const override {return loop;}
    bool start(IOService *provider) override {
        observe("service start",0,0);
        if (!IOService::start(provider)) {observe("service start",kIOReturnError,0);return false;}
        if (!dsi.map(PAGANI_DSI_BASE,0x1000)) {
            observe("DSI map",kIOReturnNoMemory,0);cleanup();IOService::stop(provider);return false;
        }
        uint32_t control=PeripheralWindow::read(&dsi,4);
        if (!(control&1)) {
            observe("DSI disabled",kIOReturnNotReady,control);
            cleanup();IOService::stop(provider);return false;
        }
        loop=IOWorkLoop::workLoop();timer=IOTimerEventSource::timerEventSource(this,tick);
        added=loop && timer && loop->addEventSource(timer)==kIOReturnSuccess;
        if (!added) {observe("timer",kIOReturnNoResources,0);cleanup();IOService::stop(provider);return false;}
        __atomic_store_n(&running,true,__ATOMIC_RELEASE);tick(this,nullptr);
        if (__atomic_load_n(&failed,__ATOMIC_ACQUIRE)) {cleanup();IOService::stop(provider);return false;}
        // Publish only with real AppleARMBacklight ancestry: CoreBrightness uses IOObjectConformsTo.
        setProperty("backlight-control",true);setProperty("Built-In",true);
        setProperty("PanelMaxBrightness",PAGANI_BRIGHTNESS_MAX,32);
        setProperty("PanelRefreshSource","firmware timing; software presentation at 60 Hz");
        registerService();return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        auto dict=OSDynamicCast(OSDictionary,properties);
        if (!dict) return kIOReturnBadArgument;
        auto raw=dict->getObject("brightness");
        if (!raw) return kIOReturnSuccess;
        auto value=OSDynamicCast(OSNumber,raw);
        if (!value || value->unsigned64BitValue()>PAGANI_BRIGHTNESS_MAX) return kIOReturnBadArgument;
        if (!__atomic_load_n(&running,__ATOMIC_ACQUIRE) || __atomic_load_n(&failed,__ATOMIC_ACQUIRE)) return kIOReturnNotReady;
        // shortcut: apply the target without FadePeriod interpolation; add fades after transport qualification.
        __atomic_store_n(&requestedLevel,value->unsigned32BitValue(),__ATOMIC_RELEASE);
        observe("brightness request",0,value->unsigned32BitValue());
        return kIOReturnSuccess;
    }
    void stop(IOService *provider) override {cleanup();IOService::stop(provider);}
    void free() override {cleanup();IOService::free();}
    void unqualifiedBacklightMethod() override {
        panic("PaganiPanel: unsupported AppleARMBacklight slot 168");
    }
};
OSDefineMetaClassAndStructors(PaganiPanel,AppleARMBacklight)
extern "C" IOService *pagani_panel_allocate(void) {return new PaganiPanel;}

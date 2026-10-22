// SPDX-License-Identifier: MIT
// hummer: publish a genuine ApplePMGRNub so the unchanged AMFI takes its
// physical-platform branch and finds a PMGR.  Without it AMFI panics with
// "AMFI: No PMGR?" @ConfigurationSettings.cpp:235: its ConfigurationSettings
// waits (IOService class match, 30 s) for an ApplePMGRNub, and hummer's
// platform expert never publishes one.  The panic site has exactly one
// caller, so publishing a real nub is a complete fix (same approach as
// UniXNU-sheng's ShengPlatformPower, qualified against the same 21H461
// kernelcache this board boots).
//
// The hook symbol is sheng_pmgr_run on purpose: upstream
// patches/xnu/sheng-platform-power.patch calls it from bsd/kern/bsd_init.c
// (after bsd_bufferinit) under #if defined(XNU_SHENG_PMGR) && XNU_SHENG_PMGR,
// and hummer_defconfig enables CONFIG_XNU_SHENG_PMGR=y.
//
// readAon/writeAon: the original kext's implementations of slots 320/321 are
// bare stubs (verified by disassembly: `mov w0,#-1; ret` / `ret`), so the C++
// overrides below are behaviorally identical and need no text bindings.
// Everything else a real PMGR backend would do is a guarded panic: nothing
// on this board is expected to call power operations, and if something does,
// the panic names the slot on the panel.
#include "OriginalPMGRABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void hummer_pmgr_static_init(void) __asm__("__GLOBAL__sub_I_HummerPlatformPower.cpp");
IOService *hummer_original_pmgr_nub_create(IORegistryEntry *, ApplePMGR *);
uint32_t hummer_original_pmgr_nub_read(IOService *);
void hummer_original_pmgr_nub_write(IOService *, uint64_t);
}

__attribute__((noreturn)) static void unavailable(unsigned slot)
{
  panic("HummerPlatformPower: unimplemented operation %u", slot);
}

class HummerPlatformPower : public ApplePMGR {
  OSDeclareDefaultStructors(HummerPlatformPower)
public:
  void enableCPUCore(unsigned, uint64_t) override { unavailable(168); }
  void enableCPUCore(unsigned) override { unavailable(169); }
  void disableCPUCore(unsigned) override { unavailable(170); }
  void enableCPUCluster(unsigned) override { unavailable(171); }
  void disableCPUCluster(unsigned) override { unavailable(172); }
  void initCPUIdle(ml_processor_info_t *) override { unavailable(173); }
  void enterCPUIdle(UInt64 *) override { unavailable(174); }
  void exitCPUIdle(UInt64 *) override { unavailable(175); }
  void updateCPUIdle(UInt64 *) override { unavailable(176); }
  void opaque177() override { unavailable(177); }
  void opaque178() override { unavailable(178); }
  void opaque179() override { unavailable(179); }
  void opaque180() override { unavailable(180); }
  void opaque181() override { unavailable(181); }
  void opaque182() override { unavailable(182); }
  void opaque183() override { unavailable(183); }
  void opaque184() override { unavailable(184); }
  void opaque185() override { unavailable(185); }
  void opaque186() override { unavailable(186); }
  void opaque187() override { unavailable(187); }
  void opaque188() override { unavailable(188); }
  void opaque189() override { unavailable(189); }
  void opaque190() override { unavailable(190); }
  void opaque191() override { unavailable(191); }
  void opaque192() override { unavailable(192); }
  void opaque193() override { unavailable(193); }
  void opaque194() override { unavailable(194); }
  void opaque195() override { unavailable(195); }
  void opaque196() override { unavailable(196); }
  void opaque197() override { unavailable(197); }
  void opaque198() override { unavailable(198); }
  void opaque199() override { unavailable(199); }
  void opaque200() override { unavailable(200); }
  void opaque201() override { unavailable(201); }
  void opaque202() override { unavailable(202); }
  void opaque203() override { unavailable(203); }
  void opaque204() override { unavailable(204); }
  void opaque205() override { unavailable(205); }
  void opaque206() override { unavailable(206); }
  void opaque207() override { unavailable(207); }
  void opaque208() override { unavailable(208); }
  void opaque209() override { unavailable(209); }
  void opaque210() override { unavailable(210); }
  void opaque211() override { unavailable(211); }
  void opaque212() override { unavailable(212); }
  void opaque213() override { unavailable(213); }
  void opaque214() override { unavailable(214); }
  void opaque215() override { unavailable(215); }
  void opaque216() override { unavailable(216); }
  void opaque217() override { unavailable(217); }
  void opaque218() override { unavailable(218); }
  void opaque219() override { unavailable(219); }
  void opaque220() override { unavailable(220); }
  void opaque221() override { unavailable(221); }
  void opaque222() override { unavailable(222); }
  void opaque223() override { unavailable(223); }
  void opaque224() override { unavailable(224); }
  void opaque225() override { unavailable(225); }
  void opaque226() override { unavailable(226); }
  void opaque227() override { unavailable(227); }
  void opaque228() override { unavailable(228); }
  void opaque229() override { unavailable(229); }
  void opaque230() override { unavailable(230); }
  void opaque231() override { unavailable(231); }
  void opaque232() override { unavailable(232); }
  void opaque233() override { unavailable(233); }
  void opaque234() override { unavailable(234); }
  void opaque235() override { unavailable(235); }
  void opaque236() override { unavailable(236); }
  void opaque237() override { unavailable(237); }
  void opaque238() override { unavailable(238); }
  void opaque239() override { unavailable(239); }
  void opaque240() override { unavailable(240); }
  void opaque241() override { unavailable(241); }
  void opaque242() override { unavailable(242); }
  void opaque243() override { unavailable(243); }
  void opaque244() override { unavailable(244); }
  void opaque245() override { unavailable(245); }
  void opaque246() override { unavailable(246); }
  void opaque247() override { unavailable(247); }
  void opaque248() override { unavailable(248); }
  void opaque249() override { unavailable(249); }
  void opaque250() override { unavailable(250); }
  void opaque251() override { unavailable(251); }
  void opaque252() override { unavailable(252); }
  void opaque253() override { unavailable(253); }
  void opaque254() override { unavailable(254); }
  void opaque255() override { unavailable(255); }
  void opaque256() override { unavailable(256); }
  void opaque257() override { unavailable(257); }
  void opaque258() override { unavailable(258); }
  void opaque259() override { unavailable(259); }
  void opaque260() override { unavailable(260); }
  void opaque261() override { unavailable(261); }
  void opaque262() override { unavailable(262); }
  void opaque263() override { unavailable(263); }
  void opaque264() override { unavailable(264); }
  void opaque265() override { unavailable(265); }
  void opaque266() override { unavailable(266); }
  void opaque267() override { unavailable(267); }
  void opaque268() override { unavailable(268); }
  void opaque269() override { unavailable(269); }
  void opaque270() override { unavailable(270); }
  void opaque271() override { unavailable(271); }
  void opaque272() override { unavailable(272); }
  void opaque273() override { unavailable(273); }
  void opaque274() override { unavailable(274); }
  void opaque275() override { unavailable(275); }
  void opaque276() override { unavailable(276); }
  void opaque277() override { unavailable(277); }
  void opaque278() override { unavailable(278); }
  void opaque279() override { unavailable(279); }
  void opaque280() override { unavailable(280); }
  void opaque281() override { unavailable(281); }
  void opaque282() override { unavailable(282); }
  void opaque283() override { unavailable(283); }
  void opaque284() override { unavailable(284); }
  void opaque285() override { unavailable(285); }
  void opaque286() override { unavailable(286); }
  void opaque287() override { unavailable(287); }
  void opaque288() override { unavailable(288); }
  void opaque289() override { unavailable(289); }
  void opaque290() override { unavailable(290); }
  void opaque291() override { unavailable(291); }
  void opaque292() override { unavailable(292); }
  void opaque293() override { unavailable(293); }
  void opaque294() override { unavailable(294); }
  void opaque295() override { unavailable(295); }
  void opaque296() override { unavailable(296); }
  void opaque297() override { unavailable(297); }
  void opaque298() override { unavailable(298); }
  void opaque299() override { unavailable(299); }
  void opaque300() override { unavailable(300); }
  void opaque301() override { unavailable(301); }
  void opaque302() override { unavailable(302); }
  void opaque303() override { unavailable(303); }
  void opaque304() override { unavailable(304); }
  void opaque305() override { unavailable(305); }
  void opaque306() override { unavailable(306); }
  void opaque307() override { unavailable(307); }
  void opaque308() override { unavailable(308); }
  void opaque309() override { unavailable(309); }
  void opaque310() override { unavailable(310); }
  void opaque311() override { unavailable(311); }
  void opaque312() override { unavailable(312); }
  void opaque313() override { unavailable(313); }
  void opaque314() override { unavailable(314); }
  void opaque315() override { unavailable(315); }
  void opaque316() override { unavailable(316); }
  void opaque317() override { unavailable(317); }
  void opaque318() override { unavailable(318); }
  void opaque319() override { unavailable(319); }
  void opaque322() override { unavailable(322); }
  void opaque323() override { unavailable(323); }
  void opaque324() override { unavailable(324); }
  void opaque325() override { unavailable(325); }
  void opaque326() override { unavailable(326); }
  void opaque327() override { unavailable(327); }
  void opaque328() override { unavailable(328); }
  void opaque329() override { unavailable(329); }
  void opaque330() override { unavailable(330); }
  void opaque331() override { unavailable(331); }
  void opaque332() override { unavailable(332); }
  void opaque333() override { unavailable(333); }
  void opaque334() override { unavailable(334); }
  void opaque335() override { unavailable(335); }
  void opaque336() override { unavailable(336); }
  void opaque337() override { unavailable(337); }
  void opaque338() override { unavailable(338); }
  void opaque339() override { unavailable(339); }
  uint32_t readAon() override { return 0xffffffffu; }
  void writeAon(uint64_t) override { }
};
OSDefineMetaClassWithInit(HummerPlatformPower, ApplePMGR, )
OSMetaClassConstructorInit(HummerPlatformPower, ApplePMGR, )
OSDefineBasicStructors(HummerPlatformPower, ApplePMGR)
// The original object's storage exceeds kalloc_type's 16 KiB size limit.
// Use XNU's existing zeroed, size-aware OSObject heap allocation pair.
void *HummerPlatformPower::operator new(size_t size) { return OSObject::operator new(size); }
void HummerPlatformPower::operator delete(void *memory, size_t size)
{
  OSObject::operator delete(memory, size);
}
static_assert(sizeof(HummerPlatformPower) == 0x4b8a8, "Exact original provider storage");

extern "C" void sheng_pmgr_run(void)
{
  kprintf("HUM PMGR 00: enter\n");
  kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
  kprintf("HUM PMGR 01: load=0x%x\n", result);
  if (result) return;

  const OSSymbol *pmgrName = OSSymbol::withCString("ApplePMGR");
  const OSSymbol *nubName = OSSymbol::withCString("ApplePMGRNub");
  const OSMetaClass *pmgrMeta = pmgrName ? OSMetaClass::getMetaClassWithName(pmgrName) : 0;
  const OSMetaClass *nubMeta = nubName ? OSMetaClass::getMetaClassWithName(nubName) : 0;
  kprintf("HUM PMGR 02: pmgr=%p nub=%p\n", pmgrMeta, nubMeta);
  if (!pmgrMeta || pmgrMeta != &ApplePMGR::gMetaClass ||
      pmgrMeta->getClassSize() != 0x4b8a8 ||
      !nubMeta || nubMeta->getClassSize() != 0x740) {
    kprintf("HUM PMGR 03: meta/size mismatch, giving up\n");
    return;
  }

  // Our metaclass registers here, deferred on purpose: its super metaclass
  // lives in the prelinked ApplePMGR kext, which must be constructed first.
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) {
    kprintf("HUM PMGR 04: preModLoad failed\n");
    return;
  }
  hummer_pmgr_static_init();
  result = OSMetaClass::postModLoad(handle);
  if (result) {
    kprintf("HUM PMGR 04: postModLoad=0x%x\n", result);
    return;
  }

  HummerPlatformPower *provider = new HummerPlatformPower;
  IORegistryEntry *description = new IORegistryEntry;
  IOService *nub = 0;
  do {
    if (!provider || !description || !provider->init() || !description->init()) {
      kprintf("HUM PMGR 05: provider/description init failed\n");
      break;
    }
    // Empty resource description: no Apple register map or device
    // capabilities; the original nub code handles that (reads return -1).
    description->setName("hummer-platform-power");
    nub = hummer_original_pmgr_nub_create(description, provider);
    kprintf("HUM PMGR 06: nub=%p\n", nub);
    if (!nub || nub->getMetaClass() != nubMeta ||
        !provider->metaCast("ApplePMGR")) {
      kprintf("HUM PMGR 07: create/cast failed\n");
      break;
    }
    uintptr_t bound = 0;
    memcpy(&bound, reinterpret_cast<const char *>(nub) + 0x88, sizeof(bound));
    if (bound != reinterpret_cast<uintptr_t>(provider)) {
      kprintf("HUM PMGR 08: binding=%p\n", (void *)bound);
      break;
    }
    uint32_t value = hummer_original_pmgr_nub_read(nub);
    hummer_original_pmgr_nub_write(nub, uint64_t(value) | 4);
    uint32_t after = hummer_original_pmgr_nub_read(nub);
    kprintf("HUM PMGR 09: read=0x%x after=0x%x\n", value, after);
    if (value != 0xffffffffu || after != 0xffffffffu) {
      kprintf("HUM PMGR 09: unexpected AON read, giving up\n");
      break;
    }

    IOService *platform = IOService::getPlatform();
    kprintf("HUM PMGR 10: platform=%p\n", platform);
    if (!platform || !provider->attach(platform)) {
      kprintf("HUM PMGR 11: provider attach failed\n");
      break;
    }
    if (!provider->start(platform)) {
      kprintf("HUM PMGR 12: provider start failed\n");
      provider->detach(platform);
      break;
    }
    if (!nub->attach(provider)) {
      kprintf("HUM PMGR 13: nub attach failed\n");
      provider->stop(platform);
      provider->detach(platform);
      break;
    }
    if (!nub->start(provider)) {
      kprintf("HUM PMGR 14: nub start failed\n");
      nub->detach(provider);
      provider->stop(platform);
      provider->detach(platform);
      break;
    }
    provider->registerService(kIOServiceSynchronous);
    nub->registerService(kIOServiceSynchronous);

    // Verify real discovery with the same call shape AMFI uses.
    OSDictionary *matching = IOService::serviceMatching("ApplePMGRNub");
    IOService *matched = matching ?
        IOService::waitForMatchingService(matching, 1000000ULL) : 0;
    kprintf("HUM PMGR 15: matched=%p\n", matched);
    if (matched != nub) {
      kprintf("HUM PMGR 15: publication verification failed\n");
      if (matching) matching->release();
      if (matched) matched->release();
      panic("HummerPlatformPower: original nub publication failed");
    }
    if (matching) matching->release();
    if (matched) matched->release();
    kprintf("HUM PMGR 19: ApplePMGRNub published\n");
    return;
  } while (false);
  kprintf("HUM PMGR 99: not published (see marker above)\n");
}

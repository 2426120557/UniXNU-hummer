// SPDX-License-Identifier: BSD-2-Clause-Patent
// Power and volume buttons of the hummer board, reported as report 2 bits of the offline HID
// device (bit 1 Power, bit 2 Volume Increment, bit 3 Volume Decrement - the layout
// ShengHidInput defines, and the one drivers/NativeHidInput now carries).
//
// Follows UniXNU-sheng's package/ButtonProbe.cpp; only the board constants differ:
//   hummer.dts  gpio_keys names GPIO6 (PM8550) as vol_down, but on this board GPIO6 is the
//               VOLUME-UP key and PON RESIN is VOLUME-DOWN - the same pairing ShengHidInput
//               uses, and the one the hardware actually shows (volume came out reversed before
//               the swap). pinctrl@8800 "gpio6" input-enable / bias-pull-up / power-source 1.
//               pon_hlos@1300 "qcom,pmk8350-pwrkey" (PON pwrkey) and a resin node
// PM8550 is SPMI USID 1, pad 5 = 0x8800 + 5*0x100, real-time status at +0x10 -> 0x8D10,
// ppid = 0x18D. The PMK8550 PON peripheral is USID 0, periph 0x13 -> RT_STS 0x1310,
// ppid = 0x013, with bit 7 "pwrkey" and bit 6 "resin".
//
// This version polls instead of taking the PMIC interrupt (arbiter -> PDC pin 1 -> SPI 746 on
// this board, 481 on sheng): the levels are what matters first, and an interrupt handler that
// is armed wrongly hides the reason a key does nothing. 20 ms sampling with a 3-sample
// debounce is well inside the DTS' 32 ms.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "BootResetCore.h"
#include "HummerTouchBoard.h"
}

// Filled in by the Consumer Control device (drivers/NativeHidInput): bit 0 = a client has
// the device open, bits 8..15 = the last handleReport result. A report nobody consumes and a
// report the device refused look identical from this side without it.
extern "C" uint32_t native_buttons_status(void) __attribute__((weak_import));

namespace {
constexpr uint32_t kPonPpid = 0x013, kGpioPpid = 0x18d;
constexpr uint8_t kPowerBit = 1u << 7, kResinBit = 1u << 6, kGpioBit = 1u << 0;
constexpr uint8_t kRtSts = 0x10;
constexpr uint32_t kReportPower = 1u << 1, kReportVolumeUp = 1u << 2, kReportVolumeDown = 1u << 3;
// Where a power key can hide, all read-only. hummer.dts puts pwrkey on interrupt 7 and resin on
// interrupt 6 of PON peripheral 0x13 (SID 0), so PON_INT_RT_STS at 0x1310 carries one bit per
// interrupt - but bit 7 stayed clear through v134, and that build never actually got a key press
// (it panicked mid-boot). Rather than trust the bit positions, read the whole PON status page
// and the real-time status of all twelve GPIO pads on both PMICs, and report what moved.
constexpr unsigned kPonProbes = 16;             // PON peripheral offsets 0x10..0x1f
constexpr unsigned kPads = 12;                  // PMIC GPIO1..12
constexpr uint8_t kPonFirstOffset = 0x10;
constexpr uint32_t kPadsPm8550 = 0x188;         // SID 1, pad 1 page (pad n = periph 0x88+n-1)
constexpr uint32_t kPadsPmk8550 = 0x0b8;        // SID 0, pad 1 page

struct Window { IOMemoryMap *map; volatile uint32_t *va; };
Window gCore, gCnfg, gObs;
br_arbiter gArb;
int gPon = -1, gGpio = -1;
uint32_t gApids;
void (*volatile gSink)(void *, uint32_t);
void *volatile gSinkContext;

uint32_t gVersion;
int gVerbose;
int gMenuProbe;                 // hummer-key-menu=1: report every key as Consumer Menu (Home)

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

// The APID serving `ppid`, as the mainline driver picks it: first match, replaced by one this
// execution environment owns. Reads only - nothing here can move a rail.
int findApid(uint32_t ppid) {
    int found = -1;
    for (uint32_t i = 0; i < gApids; ++i) {
        const uint32_t map = gCore.va[(BR_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        if (found < 0 || (gCnfg.va[(BR_CNFG_OWNER + 4 * i) / 4] & 7u) == gArb.ee) found = (int)i;
    }
    return found;
}

uint8_t gPonLast, gGpioLast;
uint32_t gSent;
int gPadApid[2][kPads];          // [0] PM8550 (SID 1), [1] PMK8550 (SID 0)
uint8_t gPonFirst[kPonProbes], gPadFirst[2][kPads];
uint16_t gPonChanged, gPadChanged[2], gPadLow[2];
bool gSwept;

// Report-2 bits for the keys currently held.
uint32_t pressed() {
    uint8_t pon = 0, gpio = 0xff;
    uint32_t mask = 0;
    if (gPon >= 0 && !br_read8(gObs.va, &gArb, gPon, kRtSts, &pon)) {
        if (pon & kPowerBit) mask |= kReportPower;
        // Board convention matches the upstream ShengHidInput reference and the observed
        // hardware: PM8550 GPIO6 is VOLUME UP, PON RESIN is VOLUME DOWN. The original
        // HummerButtons assumed hummer.dts's gpio-keys child made GPIO6 volume-down; on this
        // board that produced reversed volume, so these two lines are swapped from that.
        if (pon & kResinBit) mask |= kReportVolumeDown;
    }
    if (gGpio >= 0 && !br_read8(gObs.va, &gArb, gGpio, kRtSts, &gpio) && !(gpio & kGpioBit)) {
        mask |= kReportVolumeUp;
    }
    gPonLast = pon;
    gGpioLast = gpio;
    return mask;
}

bool setup() {
    gArb.ee = 0;
    if (!mapWindow(gCore, TB_SPMI_CORE_BASE, 0x3000) ||
        !mapWindow(gCnfg, 0xc42d000ULL, 0x4000)) return false;
    gArb.core = gCore.va;
    gArb.cnfg = gCnfg.va;
    gVersion = gCore.va[0];
    gApids = gCore.va[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
    if (gVersion < BR_VERSION_V7_MIN || gVersion >= BR_VERSION_V8_MIN || !gApids || gApids > 1024) return false;
    if (!mapWindow(gObs, TB_SPMI_OBS_BASE,
                   ((uint64_t)BR_OBS_APID_STRIDE * gApids + 0xfff) & ~0xfffull)) return false;
    gPon = findApid(kPonPpid);
    gGpio = findApid(kGpioPpid);
    for (unsigned s = 0; s < 2; ++s) {
        const uint32_t base = s == 0 ? kPadsPm8550 : kPadsPmk8550;
        for (unsigned n = 0; n < kPads; ++n) gPadApid[s][n] = findApid(base + n);
    }
    return gPon >= 0 || gGpio >= 0;
}

// One read-only pass over every candidate. The first pass records baselines; later passes only
// compare, so the returned flag means "something changed since the baseline", i.e. a key moved.
bool sweep() {
    bool moved = false;
    uint8_t v;
    for (unsigned i = 0; i < kPonProbes; ++i) {
        if (gPon < 0 || br_read8(gObs.va, &gArb, gPon, static_cast<uint8_t>(kPonFirstOffset + i), &v)) continue;
        if (!gSwept) gPonFirst[i] = v;
        else if (v != gPonFirst[i]) {
            gPonChanged = static_cast<uint16_t>(gPonChanged | (1u << i));
            moved = true;
        }
    }
    for (unsigned s = 0; s < 2; ++s) {
        for (unsigned n = 0; n < kPads; ++n) {
            const int apid = gPadApid[s][n];
            if (apid < 0 || br_read8(gObs.va, &gArb, apid, kRtSts, &v)) continue;
            if (!gSwept) gPadFirst[s][n] = v;
            else {
                if (v != gPadFirst[s][n]) {
                    gPadChanged[s] = static_cast<uint16_t>(gPadChanged[s] | (1u << n));
                    moved = true;
                }
                if (!(v & 1u)) gPadLow[s] = static_cast<uint16_t>(gPadLow[s] | (1u << n));
            }
        }
    }
    gSwept = true;
    return moved;
}

void buttonsThread(void *, wait_result_t) {
    const bool ready = setup();
    if (!ready) {
        if (gVerbose) {
            panic("HUMTOUCH btn setup failed ver%08x apids%u pon%d gpio%d",
                  gVersion, gApids, gPon, gGpio);
        }
        return;
    }
    uint32_t last = 0, candidate = 0, masks = 0;
    unsigned settled = 0, ticks = 0, presses = 0, lastMove = 0;
    bool everMoved = false;
    for (;;) {
        const uint32_t mask = pressed();
        if (sweep()) {
            everMoved = true;
            lastMove = ticks;
        }
        if (mask == candidate) {
            if (++settled >= 3 && mask != last) {
                last = mask;
                // hummer-key-menu=1 replaces whatever the key means with Consumer Menu, so a
                // single press tells us whether Consumer events from this device reach iOS at
                // all - Home works without an audio route, "Power 0x30" and volume do not.
                const uint32_t send = (gMenuProbe && mask) ? 1u : mask;
                void (*sink)(void *, uint32_t) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
                if (sink) sink(gSinkContext, send);
                if (sink) ++gSent;
                if (send) {
                    ++presses;
                    masks |= send;
                }
            }
        } else {
            candidate = mask;
            settled = 0;
        }
        // hummer-keyscan=2: the desktop is up roughly 40 s in, so wait 120 s before forcing the
        // readout - or 30 s after the last probe moved, so the effect of a press is visible
        // before the panic covers the screen.
        if (gVerbose && ((everMoved && ticks - lastMove >= 1500) || ticks >= 6000)) {
            const uint32_t st = (&native_buttons_status) ? native_buttons_status() : 0xffffffffu;
            panic("HUMTOUCH btn2 p%u m%08x st%04x\n"
                  "HUMTOUCH btn2 pon chg%04x v%02x\n"
                  "HUMTOUCH btn2 p1 chg%04x low%04x\n"
                  "HUMTOUCH btn2 p0 chg%04x low%04x",
                  presses, masks, st,
                  gPonChanged, gPonFirst[0],
                  gPadChanged[0], gPadLow[0],
                  gPadChanged[1], gPadLow[1]);
        }
        ++ticks;
        IOSleep(20);
    }
}

}  // namespace

// Called by drivers/NativeHidInput once its device is started (weak import there, so a build
// without this file still links).
extern "C" void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    gSinkContext = context;
    __atomic_store_n(&gSink, sink, __ATOMIC_RELEASE);
}

extern "C" void hummer_buttons_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("hummer-keyscan", &enabled, sizeof enabled) || !enabled) return;
    gVerbose = enabled >= 2 ? 1 : 0;
    unsigned menu = 0;
    if (PE_parse_boot_argn("hummer-key-menu", &menu, sizeof menu) && menu) gMenuProbe = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(buttonsThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

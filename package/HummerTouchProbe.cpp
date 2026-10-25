// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of the Synaptics S3910 touch controller over GENI
// SPI4 for hummer (SM8750). `hummer-touch=<n>`:
//   1  read-only snapshot of the GCC branches, the S4 DFS levels and the SPI pins
//   2  vote GPLL0 and the QUP clocks, wait for them, mux the pins, read the engine
//      identity (protocol / FIFO geometry / fifo_disabled flag)
//   3  GENI SPI init and clock, reset the touch controller, read the TCM IDENTIFY
//      report ("S3910", application mode)
//   4  GET_APP_INFO, report configuration, ENABLE_REPORT, then poll TOUCH reports
//      and publish finger sets to the NativeHidInput digitizer mailbox
// `hummer-touch-panic=<n>` panics after every stage k <= n (and on any failure)
// with the recorded registers: the panic screen is the readout, the same way the
// storage probe was read. Every protocol step is a faithful port of
// drivers/input/touchscreen/synaptics-s3910.c (AnyLaySys/linux_oneplus_ace5_pro);
// board constants and their sources are in HummerTouchBoard.h.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GeniSpi.h"
#include "HummerTouchBoard.h"
}

// NativeHidInput's touch mailbox.
//
// These are WEAK DEFINITIONS, not weak_import declarations. weak_import works
// for functions (an absent symbol makes the address 0, which is what
// `if (&native_touch_probe)` relies on), but the linker still rejects an
// undefined weak_import DATA symbol. That is exactly what broke the kernel link
// while NativeHidInput is not part of this board's build:
//     Undefined symbols for architecture arm64:
//       "_native_touch_seq", referenced from:
//           publish(...) in libHummerTouch.a(HummerTouchProbe.o)
//
// Defining them weakly keeps the archive self-contained either way:
//   - NativeHidInput not linked: these are the only definitions, so publish()
//     writes here and is a harmless no-op. The panic screen still shows the
//     real hardware state, which is what stage 1-4 bring-up reads.
//   - NativeHidInput linked: its strong definitions win and the linker discards
//     these, so publish() feeds the real HID digitizer.
extern "C" {
__attribute__((weak)) volatile uint32_t native_touch_seq;
__attribute__((weak)) volatile uint32_t native_touch_count;
__attribute__((weak)) volatile int native_touch_xs[12];
__attribute__((weak)) volatile int native_touch_ys[12];
__attribute__((weak)) volatile int native_touch_ids[12];
}

// The digitizer consumer (NativeHidInput::touchNotify) registers here; called from
// the thread that published, after the seqlock closes. Same contract as sheng.
static void (*gTouchSink)(void *);
static void *gTouchSinkContext;
extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {
    gTouchSinkContext = context;
    __atomic_store_n(&gTouchSink, sink, __ATOMIC_RELEASE);
}

extern "C" {
volatile uint32_t native_htouch_stage_req, native_htouch_stage_done, native_htouch_step;
volatile int native_htouch_error;
volatile uint32_t native_htouch_frames, native_htouch_contacts, native_htouch_again,
    native_htouch_other, native_htouch_bad, native_htouch_hid_feeds;
volatile int native_htouch_last_x = -1, native_htouch_last_y = -1;
// NativeHidInput.cpp declares this extern and increments it per HID report it
// emits from the mailbox below; the sheng kit defines it in TouchProbe.cpp.
volatile uint32_t native_touch_hid_reports;
}

// ---- S3910 TouchComm constants (synaptics-s3910.c) ----
#define S3910_MAX_PAYLOAD 4096
#define S3910_MAX_OBJECTS 10
#define S3910_MARKER 0xa5
#define S3910_PADDING 0x5a
#define S3910_STATUS_OK 0x01
#define S3910_CONTINUED_READ 0x03
#define S3910_IDENTIFY 0x02
#define S3910_ENABLE_REPORT 0x05
#define S3910_DISABLE_REPORT 0x06
#define S3910_GET_APP_INFO 0x20
#define S3910_SET_DYNAMIC_CONFIG 0x24
#define S3910_GET_REPORT_CONFIG 0x25
#define S3910_SET_REPORT_CONFIG 0x26
#define S3910_EXIT_SLEEP 0x2d
#define S3910_REPORT_IDENTIFY 0x10
#define S3910_REPORT_TOUCH 0x11
#define S3910_REPORT_HBP 0x23
#define S3910_CONFIG_LBP_HBP 0x11
// Report configuration the Linux driver programs (13 bytes).
static const uint8_t s3910_report_config[] = {
    0x18, 8,
    0x01,
    0x06, 8,
    0x07, 8,
    0x08, 16,
    0x09, 16,
    0x03,
    0x00,
};
#define TCM_EAGAIN (-11)
#define TCM_EIO (-5)
#define TCM_ENOMEM (-12)
#define TCM_EINVAL (-22)
#define TCM_ENODEV (-19)
#define TCM_EPROTO (-71)
#define TCM_EMSGSIZE (-90)
#define TCM_ETIMEDOUT (-110)

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// The serial engine's DMA master reads and writes two buffers. hummer's spi4 node
// carries no iommus property, so the master addresses physical memory directly -
// no SMMU programming, unlike sheng whose spi4 sat behind apps_smmu 0xa3. The
// buffer is allocated cached (kIOMapInhibitCache on RAM above 4 GiB panics in
// phystokv_range), so the engine's view is kept coherent with explicit maintenance.
constexpr size_t kDmaBytes = 8192;   // every TCM transfer is <= S3910_MAX_PAYLOAD + 3
void cacheRange(void *p, size_t len, int op) {   // 0 clean, 1 clean+invalidate, 2 invalidate
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1), end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
void dmaClean(void *, void *p, size_t len) {cacheRange(p, len, 0);}
void dmaCleanInval(void *, void *p, size_t len) {cacheRange(p, len, 1);}
void dmaInval(void *, void *p, size_t len) {cacheRange(p, len, 2);}

uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
// The SPI core polls its status registers with a 1 us wait between reads; spinning
// on the counter beats IODelay's two context switches per poll (sheng int-182).
void ioDelayUs(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}

struct Session {
    Region gccLow, gccHalt, gccVote, gccS4, tlmmSpi, tlmmReset, wrapper, se;
    IOBufferMemoryDescriptor *dmaMem;
    uint64_t dmaPhys;
    struct geni_spi spi;
    uint8_t *tx, *rx;                 // S3910_MAX_PAYLOAD + 3 each (heap: 16 KiB stacks)
    uint16_t max_write = 1024, config_size, max_x, max_y, objects;
    uint16_t lastLength;              // payload length of the most recent received report
    uint64_t levelHz[TB_DFS_LEVELS];
    unsigned level = 0, div = 0;
    uint64_t actualHz = 0;
    uint32_t vals[24];                // live register snapshot for the panic readout
    unsigned traceN;                  // first successful transfers on the wire
    uint32_t traceLen[8];
    uint8_t traceTx[8][6], traceRx[8][6];
    int lastRc = 0;
    uint32_t lastLen = 0;
    int fwVersion = -1;
    unsigned fwMode = 0, idRc = 0;
    bool viaCommand = false;          // IDENTIFY arrived by command, not spontaneously
    unsigned allowFifoDisabled = 1;   // hummer-touch-fifo=0 forces the GPI DMA path
};

void fail(unsigned step, int code) {
    if (!native_htouch_error && code) {native_htouch_step = step; native_htouch_error = code;}
}
}  // namespace

// ---- S3910 TouchComm protocol (faithful port of synaptics-s3910.c) ----
namespace {
int busTrace(Session &s, int rc, size_t len, const uint8_t *tx, const uint8_t *rx) {
    // Record every early success plus the FIRST failure. The failure's tx/rx bytes are
    // what separates "nothing drove MISO" (all 0xff or all 0x00, i.e. pin mux, power or
    // CS) from a real protocol mismatch. Bit 31 of traceLen marks a failed transfer.
    const bool firstFailure = rc && !s.lastRc;
    if ((!rc || firstFailure) && s.traceN < 8) {
        unsigned i = s.traceN++;
        s.traceLen[i] = static_cast<uint32_t>(len) | (rc ? 0x80000000u : 0);
        for (unsigned k = 0; k < 6 && k < len && tx; ++k) s.traceTx[i][k] = tx[k];
        for (unsigned k = 0; k < 6 && k < len && rx; ++k) s.traceRx[i][k] = rx[k];
    }
    if (firstFailure) {s.lastRc = rc; s.lastLen = static_cast<uint32_t>(len);}
    return rc;
}
int busXfer(Session &s, const uint8_t *tx, uint8_t *rx, size_t len) {
    int rc = geni_spi_transfer(&s.spi, tx, rx, len, 0);
    return busTrace(s, rc, len, tx, rx);
}
// One full-duplex read: clock `length` dummy 0xff bytes out, capture the response.
// rx is pre-filled with 0xAA so the panic trace can tell "transfer never started"
// (rx still 0xAA) from "transfer ran but slave drove MISO low/high" (rx changed).
int tcmRead(Session &s, size_t length) {
    if (!s.tx || !s.rx || length > S3910_MAX_PAYLOAD + 3) return TCM_EINVAL;
    memset(s.tx, 0xff, length);
    memset(s.rx, 0xaa, length);
    return busXfer(s, s.tx, s.rx, length);
}
// Read the response header (4 bytes: marker, code, length_le16), then length+3
// more when a payload follows. Linux reads the two halves as separate CS cycles
// and this controller accepts that, so the port does the same.
int tcmReceive(Session &s) {
    s.lastLength = 0;
    int rc = tcmRead(s, 4);
    if (rc) return rc;
    if (s.rx[0] != S3910_MARKER) return TCM_EPROTO;
    int code = s.rx[1];
    if (!code || code == 0xff) return TCM_EAGAIN;
    if (code == S3910_CONTINUED_READ) return TCM_EPROTO;
    uint16_t length = static_cast<uint16_t>(s.rx[2] | (s.rx[3] << 8));
    if (length > S3910_MAX_PAYLOAD) return TCM_EMSGSIZE;
    if (!length) return code;
    rc = tcmRead(s, static_cast<size_t>(length) + 3);
    if (rc) return rc;
    if (s.rx[0] != S3910_MARKER || s.rx[1] != S3910_CONTINUED_READ ||
        s.rx[length + 2] != S3910_PADDING)
        return TCM_EPROTO;
    s.lastLength = length;
    return code;
}
// Send a command frame [cmd][len_le16][payload] and poll for the response. On
// success s.rx holds the response payload (data begins at rx+2) and s.lastLength
// its length - the port never re-reads after a command, which would clobber the
// answer with the next idle header.
int tcmCommand(Session &s, uint8_t command, const void *payload, size_t length) {
    if (length + 3 > s.max_write) return TCM_EMSGSIZE;
    s.tx[0] = command;
    s.tx[1] = static_cast<uint8_t>(length & 0xff);
    s.tx[2] = static_cast<uint8_t>(length >> 8);
    if (length) memcpy(s.tx + 3, payload, length);
    int rc = busXfer(s, s.tx, nullptr, length + 3);
    if (rc) return rc;
    for (unsigned ms = 0; ms < 1000; ms += 6) {
        IOSleep(6);
        int code = tcmReceive(s);
        if (code == S3910_STATUS_OK || (command == S3910_IDENTIFY && code == S3910_REPORT_IDENTIFY))
            return 0;
        if (code == TCM_EAGAIN || code >= S3910_REPORT_IDENTIFY) continue;
        return code < 0 ? code : TCM_EIO;
    }
    return TCM_ETIMEDOUT;
}
// Reset pulse (active-low GPIO 161): assert 10 ms, release 200 ms; the controller
// then volunteers its IDENTIFY report (Linux: gpiod 1 -> physical low, 10 ms,
// gpiod 0 -> physical high, 200 ms, then receive).
int tcmResetAndIdentify(Session &s) {
    wr32(s.tlmmReset.va, 0, tb_pin_cfg_out(TB_DRIVE_2MA));
    wr32(s.tlmmReset.va, 4, tb_gpio_out_io(0));     // physical low: reset asserted
    IOSleep(10);
    wr32(s.tlmmReset.va, 4, tb_gpio_out_io(1));     // physical high: reset released
    IOSleep(200);
    int code = tcmReceive(s);
    if (code != S3910_REPORT_IDENTIFY) {
        s.viaCommand = true;
        int rc = tcmCommand(s, S3910_IDENTIFY, nullptr, 0);
        if (rc) return rc;
    } else {
        s.viaCommand = false;
    }
    // Payload starts at rx+2 of the accepted body read (marker, continued-read, data).
    const uint8_t *data = s.rx + 2;
    if (s.lastLength < 24 || memcmp(data + 2, "S3910", 5)) return TCM_ENODEV;
    if (data[1] != 1) {s.fwMode = data[1]; return TCM_ENODEV;}   // not in application mode
    s.fwMode = data[1];
    s.fwVersion = static_cast<int>(static_cast<unsigned>(data[18]) | (static_cast<unsigned>(data[19]) << 8) |
        (static_cast<unsigned>(data[20]) << 16) | (static_cast<unsigned>(data[21]) << 24));
    uint16_t deviceMax = static_cast<uint16_t>(data[22] | (data[23] << 8));
    if (deviceMax) s.max_write = s.max_write < deviceMax ? s.max_write : deviceMax;
    return 0;
}
int tcmConfigure(Session &s) {
    const uint8_t mode[] = {S3910_CONFIG_LBP_HBP, 2, 0};
    uint8_t report = S3910_REPORT_HBP;
    int rc = tcmCommand(s, S3910_DISABLE_REPORT, &report, 1);
    if (rc) return rc;
    report = S3910_REPORT_TOUCH;
    rc = tcmCommand(s, S3910_DISABLE_REPORT, &report, 1);
    if (rc) return rc;
    rc = tcmCommand(s, S3910_SET_DYNAMIC_CONFIG, mode, sizeof mode);
    if (rc) return rc;
    uint8_t *config = static_cast<uint8_t *>(IOMalloc(s.config_size));
    if (!config) return TCM_ENOMEM;
    memset(config, 0, s.config_size);
    memcpy(config, s3910_report_config, sizeof s3910_report_config);
    rc = tcmCommand(s, S3910_SET_REPORT_CONFIG, config, s.config_size);
    IOFree(config, s.config_size);
    if (rc) return rc;
    rc = tcmCommand(s, S3910_GET_REPORT_CONFIG, nullptr, 0);
    if (rc) return rc;
    if (s.lastLength < static_cast<uint16_t>(sizeof s3910_report_config) ||
        memcmp(s.rx + 2, s3910_report_config, sizeof s3910_report_config))
        return TCM_EPROTO;
    rc = tcmCommand(s, S3910_EXIT_SLEEP, nullptr, 0);
    if (rc) return rc;
    report = S3910_REPORT_TOUCH;
    return tcmCommand(s, S3910_ENABLE_REPORT, &report, 1);
}
}  // namespace

// Publish the finger set (seqlock mailbox, coordinates already in HID space).
static void publish(unsigned count, const int *ids, const int *xs, const int *ys) {
    // The mailbox is always linked now (weak definitions above), so there is no
    // null-guard left to take. When NativeHidInput is absent these writes land
    // in the weak storage and are a harmless no-op - stage 1-4 bring-up reads
    // the real hardware state off the panic screen anyway. When NativeHidInput
    // is linked its strong definitions win and this feeds the HID digitizer.
    native_touch_seq = native_touch_seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count && i < 12; ++i) {
        native_touch_xs[i] = xs[i];
        native_touch_ys[i] = ys[i];
        native_touch_ids[i] = ids[i];
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    native_htouch_hid_feeds = native_htouch_hid_feeds + 1;
    if (void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE)) sink(gTouchSinkContext);
}

namespace {
// Stage 1: map everything, record registers. No writes anywhere.
bool snapshot(Session &s) {
    if (!mapRegion(TB_GCC_BASE + 0x0000, 0x1000, s.gccLow) ||
        !mapRegion(TB_GCC_BASE + 0x18000, 0x1000, s.gccS4) ||
        !mapRegion(TB_GCC_BASE + 0x23000, 0x1000, s.gccHalt) ||
        !mapRegion(TB_GCC_BASE + 0x52000, 0x1000, s.gccVote) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_SPI_MISO), 4 * TB_GPIO_STRIDE, s.tlmmSpi) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_RESET), TB_GPIO_STRIDE, s.tlmmReset)) {
        fail(1, TCM_ENOMEM);
        return false;
    }
    uint32_t *v = s.vals;
    v[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    v[1] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    v[2] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    v[3] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    v[4] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    v[5] = rd32(s.gccS4.va, TB_HALT_S4 & 0xfff);
    v[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
        uint32_t cfg = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_CFG(l)) & 0xfff);
        uint32_t m = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_M(l)) & 0xfff);
        uint32_t n = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_N(l)) & 0xfff);
        s.levelHz[l] = tb_dfs_level_hz(cfg, m, n);
        if (l < 4) v[15 + l] = cfg;
    }
    for (unsigned i = 0; i < 4; ++i) v[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    v[13] = rd32(s.tlmmReset.va, 4);
    return true;
}

// Stage 2: vote GPLL0 and the QUPv3_1 branches, mux the SPI pins, read the engine.
bool enable(Session &s) {
    // GPLL0 feeds the DFS levels: shared vote, then wait for lock.
    wr32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    IODelay(200);
    // TAYCAN_ELU alpha-PLL bring-up on PLL_MODE (offset 0x0): the 0x52020 vote only powers
    // the GDSC; the PLL stays held in reset until we release BYPASSNL(bit0) -> RESET_N(bit1),
    // wait for LOCK_DET(bit31), then OUTCTRL(bit2). Before this, PLL_MODE read 0x5
    // (BYPASSNL|OUTCTRL set, RESET_N=0): GPLL0 never locked, every DFS level clocked from it
    // delivered no SCLK, and the GenI SPI engine sat stalled (-110, RX FIFO never filled).
    wr32(s.gccLow.va, TB_GCC_GPLL0_MODE, rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) | (1u << 0));
    wr32(s.gccLow.va, TB_GCC_GPLL0_MODE, rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) | (1u << 0) | (1u << 1));
    for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    wr32(s.gccLow.va, TB_GCC_GPLL0_MODE, rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) | (1u << 0) | (1u << 1) | (1u << 2));
    s.vals[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff) | TB_VOTE_ALL);
    // Branches with hardware clock gating (bit 1) show CLK_OFF while idle and wake on
    // access; only a branch without that bit must have CLK_OFF clear.
    auto running = [](uint32_t cbcr) {return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = running(rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff)) && running(rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccS4.va, TB_HALT_S4 & 0xfff) & TB_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    s.vals[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[1] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    s.vals[2] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    s.vals[3] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    s.vals[4] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    s.vals[5] = rd32(s.gccS4.va, TB_HALT_S4 & 0xfff);
    if (!ok) {fail(2, TCM_ETIMEDOUT); return false;}   // never touch the engine without running clocks
    // One function value for all four pins: the target kernel's pinctrl-sm8750.c puts
    // qup1_se4 at funcs[1] for gpio48..51 alike (see HummerTouchBoard.h).
    const uint32_t cfg = tb_pin_cfg(TB_FUNC_QUP1_SE4, TB_DRIVE_6MA);
    for (unsigned i = 0; i < 4; ++i) wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, cfg);
    for (unsigned i = 0; i < 4; ++i) s.vals[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    if (!mapRegion(TB_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) {
        fail(2, TCM_ENOMEM);
        return false;
    }
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    int r = geni_spi_read_info(&s.spi);
    s.vals[19] = s.spi.info.qup_hw_version;
    s.vals[20] = s.spi.info.fw_revision;
    s.vals[21] = (s.spi.info.protocol << 16) | (s.spi.info.if_disable & 0xffff);
    s.vals[22] = s.spi.info.hw_param0;
    if (r) {fail(2, r); return false;}
    return true;
}

// Stage 3 bus setup: FIFO mode, GENI init, then the serial clock.
//
// The engine reports FIFO_IF_DISABLE (if_disable=1 on this DTS), which nominally means
// "use GPI DMA": a GSI channel, its iommu context and the qup-core/qup-config/qup-memory
// interconnect votes. None of that is implemented here, so the code below kept the SE's
// own DMA master instead - and that hard-resets the machine the moment a transfer starts
// (stage 3, no panic screen, even with SE_IRQ_EN cleared). GeniSpi.h exposes the Linux
// fallback for exactly this case: allow_fifo_disabled keeps the FIFO path alive although
// the engine advertises FIFO_IF_DISABLE. The DMA setup is kept but never taken, so
// switching back is one condition.
bool openBus(Session &s) {
    s.spi.poll_limit = 400000;
    // poll_only=1: geni_spi_init() clears SE_IRQ_EN (0xe1c), so the SE never asserts its
    // GIC SPI (357). This board installs no handler for that line and the ADT publishes no
    // interrupt, so an asserted 357 is a fatal "ghost handler" data abort - seen on the op7
    // build as ESR 0x96000010 / FAR 0xffffffff284708008 (Address-size fault on a truncated
    // 64-bit handler pointer). With SE_IRQ_EN=0 the line stays silent and the kernel survives.
    //
    // SE_GENI_M_IRQ_STATUS (0x610) is gated by SE_GENI_M_IRQ_EN (0x614) - which init always
    // sets - NOT by SE_IRQ_EN. So polling 0x610 still sees M_CMD_DONE fire when a transfer
    // completes. (The earlier "poll_only=1 froze M_IRQ_STATUS" note was measured only under
    // the old opcode-3 path, where the engine silently dropped the illegal command and the
    // status stayed 0; with the real opcode 7 the transfer runs and latches as expected.)
    s.spi.poll_only = 1;
    s.spi.allow_fifo_disabled = 1;   // FIFO even though FIFO_IF_DISABLE is set
    if (s.spi.info.fifo_disabled && !s.spi.allow_fifo_disabled) {
        s.dmaMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
            2 * kDmaBytes, 0xfffffff000ULL);
        if (!s.dmaMem || s.dmaMem->prepare() != kIOReturnSuccess) {fail(3, TCM_ENOMEM); return false;}
        addr64_t seg_len = 0;
        s.dmaPhys = s.dmaMem->getPhysicalSegment(0, &seg_len);
        uint8_t *virt = static_cast<uint8_t *>(s.dmaMem->getBytesNoCopy());
        if (!virt || !s.dmaPhys || seg_len < 2 * kDmaBytes) {fail(3, TCM_EIO); return false;}
        memset(virt, 0, 2 * kDmaBytes);
        cacheRange(virt, 2 * kDmaBytes, 1);
        s.spi.use_dma = 1;
        s.spi.dma.ctx = nullptr;
        s.spi.dma.clean = dmaClean;
        s.spi.dma.clean_inval = dmaCleanInval;
        s.spi.dma.inval = dmaInval;
        s.spi.dma.tx_virt = virt;
        s.spi.dma.rx_virt = virt + kDmaBytes;
        s.spi.dma.tx_phys = s.dmaPhys;
        s.spi.dma.rx_phys = s.dmaPhys + kDmaBytes;
        s.spi.dma.size = kDmaBytes;
    }
    // v82: at reset the SE FIFO interface is disabled (SE_FIFO_IF_DISABLE 0x2008 bit0=1;
    // read-only mirror is GENI_IF_DISABLE_RO 0x64). Mainline spi-geni-qcom, when no GPI DMA
    // channel is available, falls back to FIFO and clears this bit via
    // qcom-geni-se geni_se_select_mode(FIFO)->clrbits(SE_FIFO_IF_DISABLE). The XNU GeniSpi
    // component only honours the flag in software and never touches the hardware bit, so the
    // FIFO path stayed physically disabled -> RX FIFO never filled (rxfs=0, irq=0, -110).
    // Clear it here before init so read_info already sees FIFO enabled.
    s.vals[17] = rd32(s.se.va, 0x64);
    wr32(s.se.va, 0x2008, rd32(s.se.va, 0x2008) & ~1u);   // clear FIFO_IF_DISABLE
    s.vals[18] = rd32(s.se.va, 0x64);                      // expect bit0 now 0
    int r = geni_spi_init(&s.spi);
    if (r) {fail(3, r); return false;}
    return true;
}

bool configureClock(Session &s, unsigned hz) {
    struct tb_clock_choice choice = {};
    if (tb_choose_clock(s.levelHz, TB_DFS_LEVELS, s.spi.oversampling, hz, &choice)) {fail(4, TCM_EINVAL); return false;}
    // v77: force TCXO-backed DFS level 0 (perf CFG=0x1, parent src=0 = TCXO 19.2MHz).
    // GPLL0 stays untouched; the level-0 RCG output is the always-on 19.2M reference.
    choice.level = 0;
    choice.sclk_hz = 19200000u;
    s.level = choice.level;
    s.div = choice.div;
    s.actualHz = choice.actual_hz;
    int r = geni_spi_set_clock(&s.spi, hz, static_cast<unsigned>(choice.sclk_hz), choice.level);
    if (r) {fail(4, r); return false;}
    r = geni_spi_setup(&s.spi, TB_SPI_MODE, 0);
    if (r) {fail(4, r); return false;}
    return true;
}

// Panic readout: the panic screen is the only console this bring-up has.
void panicReadout(const Session &s, const char *why) {
    static char text[1200];
    const uint32_t *v = s.vals;
    snprintf(text, sizeof text,
        "HUMTOUCH %s s%u/%u st%u e%d\n"
        "HUMTOUCH vote%08x halt %08x %08x %08x %08x %08x gpll%08x pins %08x %08x %08x %08x\n"
        "HUMTOUCH qup%08x fw%08x if%08x hw%08x lvl%u div%u %uHz fd%u ud%u os%u dfs %08x %08x fifo %08x %08x\n"
        "HUMTOUCH id rc%u via%u mode%u ver%d w%u mx%u my%u obj%u cfg%u fr%u ct%u agn%u oth%u bad%u feed%u rc%d n%u\n"
        "HUMTOUCH x0 %02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x x1 %02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x\n"
        "HUMTOUCH snap irq%08x st%08x txfs%08x rxfs%08x dmatx%08x dmarx%08x valid%u",
        why, native_htouch_stage_done, native_htouch_stage_req, native_htouch_step, native_htouch_error,
        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[9], v[10], v[11], v[12],
        v[19], v[20], v[21], v[22], s.level, s.div, static_cast<unsigned>(s.actualHz),
        static_cast<unsigned>(s.spi.info.fifo_disabled), static_cast<unsigned>(s.spi.use_dma),
        static_cast<unsigned>(s.spi.oversampling), v[15], v[16], v[17], v[18],
        s.idRc, static_cast<unsigned>(s.viaCommand), s.fwMode, s.fwVersion, s.max_write, s.max_x, s.max_y,
        s.objects, s.config_size,
        native_htouch_frames, native_htouch_contacts, native_htouch_again, native_htouch_other, native_htouch_bad,
        native_htouch_hid_feeds, s.lastRc, s.lastLen,
        s.traceTx[0][0], s.traceTx[0][1], s.traceTx[0][2], s.traceTx[0][3], s.traceTx[0][4], s.traceTx[0][5],
        s.traceRx[0][0], s.traceRx[0][1], s.traceRx[0][2], s.traceRx[0][3], s.traceRx[0][4], s.traceRx[0][5],
        s.traceTx[1][0], s.traceTx[1][1], s.traceTx[1][2], s.traceTx[1][3], s.traceTx[1][4], s.traceTx[1][5],
        s.traceRx[1][0], s.traceRx[1][1], s.traceRx[1][2], s.traceRx[1][3], s.traceRx[1][4], s.traceRx[1][5],
        s.spi.snap_irq, s.spi.snap_status, s.spi.snap_txfs, s.spi.snap_rxfs,
        s.spi.snap_dma_tx, s.spi.snap_dma_rx, s.spi.snap_valid);
    panic("%s", text);
}

unsigned gPanicAfter;
void checkpoint(unsigned stage, const Session &s) {
    native_htouch_stage_done = stage;
    // "Reach stage N": stops at the first checkpoint at or past gPanicAfter, so
    // hummer-touch-panic=3 报的是 stage3 完成后的状态，而不是 stage1 就把机器停掉。
    // The sub-stage probes in touchThread use 5/6/7 for the three steps of stage 3.
    if (gPanicAfter && stage >= gPanicAfter) panicReadout(s, "done");
}
void checkpointFail(const Session &s) {
    if (gPanicAfter) panicReadout(s, "fail");
}
}  // namespace

static void touchThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "hummer-touch");
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, TCM_ENOMEM); return;}
    bzero(s, sizeof *s);
    // bzero also erases the in-struct initialisers (max_write = 1024 among them), and
    // tcmCommand() then rejects every command with -EMSGSIZE before a single byte goes
    // out - which is exactly the -90 the panic screen reported while the real failure
    // was the timed-out read before it. Restore the ones that must not be zero.
    s->max_write = 1024;
    unsigned stage = native_htouch_stage_req, hz = TB_SPI_DEFAULT_HZ, pollMs = 8;
    int mirrorY = 1, flipX = 0, swapXY = 0;
    PE_parse_boot_argn("hummer-touch-hz", &hz, sizeof hz);
    if (hz < 100000 || hz > TB_SPI_MAX_HZ) hz = TB_SPI_DEFAULT_HZ;
    PE_parse_boot_argn("hummer-touch-poll", &pollMs, sizeof pollMs);
    if (!pollMs || pollMs > 100) pollMs = 8;
    PE_parse_boot_argn("hummer-touch-fy", &mirrorY, sizeof mirrorY);
    PE_parse_boot_argn("hummer-touch-fx", &flipX, sizeof flipX);
    PE_parse_boot_argn("hummer-touch-swap", &swapXY, sizeof swapXY);
    unsigned fifoMode = 1;
    PE_parse_boot_argn("hummer-touch-fifo", &fifoMode, sizeof fifoMode);
    s->allowFifoDisabled = fifoMode ? 1 : 0;
    do {
        if (!snapshot(*s)) {checkpointFail(*s); break;}
        checkpoint(1, *s);
        if (stage < 2) break;
        if (!enable(*s)) {checkpointFail(*s); break;}
        checkpoint(2, *s);
        if (stage < 3) break;
        if (!openBus(*s)) {checkpointFail(*s); break;}
        // Sub-stage probes inside stage 3: 5 = engine initialised (DMA buffer, GENI),
        // 6 = serial clock programmed, 7 = transfer buffers allocated. Each one stops
        // the machine right before the next risky write, so a hard reset can be
        // attributed to exactly one step.
        if (gPanicAfter == 5) panicReadout(*s, "openBus done");
        if (!configureClock(*s, hz)) {checkpointFail(*s); break;}
        if (gPanicAfter == 6) panicReadout(*s, "clock done");
        s->tx = static_cast<uint8_t *>(IOMalloc(S3910_MAX_PAYLOAD + 3));
        s->rx = static_cast<uint8_t *>(IOMalloc(S3910_MAX_PAYLOAD + 3));
        if (!s->tx || !s->rx) {fail(5, TCM_ENOMEM); checkpointFail(*s); break;}
        // A read that times out leaves rx untouched; stale bytes must not parse as a
        // protocol header with a huge length.
        memset(s->rx, 0, S3910_MAX_PAYLOAD + 3);
        if (gPanicAfter == 7) panicReadout(*s, "buffers done");
        const int r = tcmResetAndIdentify(*s);
        s->idRc = static_cast<unsigned>(r);
        if (r) {fail(5, r); checkpointFail(*s); break;}
        checkpoint(3, *s);
        if (stage < 4) break;
        // GET_APP_INFO (payload at rx+2, s.lastLength set by the accepted receive):
        // status @+2 must be 0, config_size @+12, max_x @+32, max_y @+34, objects @+36.
        int rc = tcmCommand(*s, S3910_GET_APP_INFO, nullptr, 0);
        if (rc) {fail(6, rc); checkpointFail(*s); break;}
        const uint8_t *data = s->rx + 2;
        if (s->lastLength < 38 || (data[2] | (data[3] << 8))) {fail(6, TCM_ENODEV); checkpointFail(*s); break;}
        s->config_size = static_cast<uint16_t>(data[12] | (data[13] << 8));
        s->max_x = static_cast<uint16_t>(data[32] | (data[33] << 8));
        s->max_y = static_cast<uint16_t>(data[34] | (data[35] << 8));
        s->objects = static_cast<uint16_t>(data[36] | (data[37] << 8));
        if (s->config_size < sizeof s3910_report_config || s->config_size + 3 > s->max_write ||
            !s->max_x || !s->max_y || !s->objects || s->objects > S3910_MAX_OBJECTS) {
            fail(6, TCM_EINVAL); checkpointFail(*s); break;
        }
        rc = tcmConfigure(*s);
        if (rc) {fail(7, rc); checkpointFail(*s); break;}
        checkpoint(4, *s);
        // Touch loop: poll the report stream (the level-low IRQ on GPIO 162 stays
        // unconfigured; TCM consumes one report per read, so polling is lossless
        // up to the poll interval). A vanished finger is the tip-up: NativeHidInput
        // sends it from its previous-finger bookkeeping.
        for (;;) {
            int code = tcmReceive(*s);
            if (code == S3910_REPORT_TOUCH) {
                const uint8_t *d = s->rx + 2;
                const unsigned count = d[0];
                if (!s->lastLength || count > s->objects || s->lastLength != 1 + count * 6) {native_htouch_bad = native_htouch_bad + 1; continue;}
                native_htouch_frames = native_htouch_frames + 1;
                const unsigned maxX = swapXY ? s->max_y : s->max_x, maxY = swapXY ? s->max_x : s->max_y;
                int ids[12], xs[12], ys[12];
                unsigned n = 0;
                for (unsigned i = 0; i < count; ++i) {
                    const uint8_t *p = d + 1 + i * 6;
                    if (p[0] >= s->objects) continue;
                    if (p[1] != 1 && p[1] != 2) continue;   // lift: absent from the active set
                    unsigned rawX = p[2] | (p[3] << 8), rawY = p[4] | (p[5] << 8);
                    if (swapXY) {const unsigned t = rawX; rawX = rawY; rawY = t;}
                    if (rawX > maxX) rawX = maxX;
                    if (rawY > maxY) rawY = maxY;
                    // NativeHidInput normalises X by 30480 and Y by 20320; mirror Y from
                    // the other edge first (the sheng v94 panel convention, opt-out with
                    // hummer-touch-fy=0), then optional axis flips for orientation bring-up.
                    long long ux = static_cast<long long>(rawX) * TB_UI_X_MAX / (maxX ? maxX - 1 : 1);
                    long long uy = static_cast<long long>(rawY) * TB_UI_Y_MAX / (maxY ? maxY - 1 : 1);
                    if (mirrorY) uy = TB_UI_Y_MAX - uy;
                    if (flipX) ux = TB_UI_X_MAX - ux;
                    ids[n] = p[0]; xs[n] = static_cast<int>(ux); ys[n] = static_cast<int>(uy);
                    ++n;
                }
                if (n) {native_htouch_last_x = xs[0]; native_htouch_last_y = ys[0];}
                native_htouch_contacts = n;
                publish(n, ids, xs, ys);
            } else if (code == S3910_REPORT_IDENTIFY) {
                // The controller rebooted (crash/watchdog): reconfigure in place.
                const int rc = tcmConfigure(*s);
                if (rc) {fail(8, rc); checkpointFail(*s); break;}
            } else if (code == TCM_EAGAIN || code == S3910_STATUS_OK) {
                native_htouch_again = native_htouch_again + 1;   // idle: nothing pending
            } else if (code >= S3910_REPORT_IDENTIFY) {
                native_htouch_other = native_htouch_other + 1;   // an unexpected report type; consume and continue
            } else {
                native_htouch_bad = native_htouch_bad + 1;
                IOSleep(20);
            }
            IOSleep(pollMs);
        }
    } while (0);
}

extern "C" void native_touch_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("hummer-touch", &stage, sizeof stage) || stage < 1 || native_htouch_stage_req) return;
    if (stage > 4) stage = 4;
    native_htouch_stage_req = stage;
    PE_parse_boot_argn("hummer-touch-panic", &gPanicAfter, sizeof gPanicAfter);
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_htouch_error = TCM_ENOMEM;
}

// SPDX-License-Identifier: BSD-2-Clause-Patent
// apps RSC DRV-2 vote - see HummerRpmh.h. Polled, no interrupts, no libc.
#include "HummerRpmh.h"

// rpmh-rsc.c: the TCS block lives at qcom,tcs-offset (0xd00 here) inside the drv window,
// 0x2a0 apart; the apps RSC has three ACTIVE TCS (qcom,tcs-config).
#define HR_TCS_OFF 0xd00u
#define HR_TCS_STRIDE 0x2a0u
#define HR_ACTIVE_N 3u
#define HR_DRV 2u                       // the drv-2 window is at 0x16520000
#define HR_WRITE_ACK (8u | (1u << 8) | (1u << 16))
#define HR_DB_MAGIC 0x0c0330dbu
#define HR_DB_ENTRY 24u
#define HR_DB_SIZE 0x20000u

#define HR_ERR_PARAM (-22)
#define HR_ERR_LAYOUT (-95)
#define HR_ERR_CMDDb (-19)
#define HR_ERR_BUSY (-16)
#define HR_ERR_IO (-5)
#define HR_ERR_NOT_DONE (-110)

// rpmh-rsc.c ver_2_7 and ver_3_0 register offsets, relative to a TCS / command base.
struct hr_layout {
    uint32_t wait, control, status, enable, msgid, addr, data, cmd_status, cmd_stride;
};
static const struct hr_layout hr_v2 = {0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 20};
static const struct hr_layout hr_v3 = {0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40, 24};

// The cmd-db header is `version, magic, slave[8], data[]`, but how many bytes sit between
// the magic and the first slave - and where `data` starts - is exactly the kind of detail
// that differs between vendor bootloaders. v109 guessed one pair (8 / 144) and the lookup
// returned "no such resource" for both rails, so instead of guessing again the finder
// tries both documented shapes and keeps the one that produces a match:
//   slave base 8  + data 136   (sizeof(header) with 8 sixteen-byte slave slots)
//   slave base 16 + data 144   (8 bytes of reserved after the magic)
static const uint8_t hr_sbase[2] = {8u, 16u};
static const uint16_t hr_hbase[2] = {136u, 144u};

static uint32_t hr_rd(volatile uint32_t *base, uint32_t off) {
    return base[off >> 2];
}
static void hr_wr(volatile uint32_t *base, uint32_t off, uint32_t v) {
    base[off >> 2] = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
// Busy wait on the virtual counter: the TCS needs ~1 ms per command round trip and there
// is no timer to sleep on this early in the boot.
static void hr_wait_us(unsigned us) {
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}
// A readback-checked write: rpmh-rsc.c only ever touches these registers through
// readl/writel, and a write that does not stick means the window is not what we think.
static int hr_sync(struct hrpmh *h, uint32_t off, uint32_t v) {
    hr_wr(h->rsc, off, v);
    for (unsigned i = 0; i < 1000; ++i) {
        if (hr_rd(h->rsc, off) == v) return 0;
        hr_wait_us(1000);
    }
    return HR_ERR_IO;
}

static uint16_t hr_db16(struct hrpmh *h, uint32_t off) {
    return (uint16_t)(hr_rd(h->db, off & ~3u) >> (8u * (off & 3u)));
}

// Walk every slave/entry of the cmd-db with one (slave base, data base) hypothesis and
// look for the 8-byte resource key. Records what it saw so a failed flash still tells us
// whether the walk was even aligned (k0/k1 should read as printable ASCII).
static int hr_walk(struct hrpmh *h, const uint32_t key[2], unsigned sbase, unsigned hbase,
                   uint32_t *address, unsigned *walked) {
    unsigned seen = 0, matches = 0, first = 1;
    int haveAddr = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const uint32_t sh = sbase + 16u * i;
        if (!hr_db16(h, sh)) break;                     // end of the slave list
        const uint32_t entries = hbase + hr_db16(h, sh + 2);
        const unsigned count = hr_db16(h, sh + 6);
        if ((entries & 3u) || entries + count * HR_DB_ENTRY > HR_DB_SIZE) continue;
        for (unsigned j = 0; j < count; ++j) {
            const uint32_t e = entries + HR_DB_ENTRY * j;
            const uint32_t k0 = hr_rd(h->db, e), k1 = hr_rd(h->db, e + 4);
            if (first) {
                first = 0;
                h->diag.k0 = k0;
                h->diag.k1 = k1;
            }
            ++seen;
            if (k0 != key[0] || k1 != key[1]) continue;
            const uint32_t a = hr_rd(h->db, e + 16);
            ++matches;
            // Prefer a 16-byte aligned address (a PMIC4 VRM slot); keep the first match
            // as a fallback so a different encoding still gets a vote.
            if (!(a & 0xfu) && !haveAddr) {*address = a; haveAddr = 1;}
            if (!haveAddr) *address = a;
        }
    }
    *walked = seen;
    h->diag.found = matches;
    return matches ? 1 : 0;
}

static int hr_find(struct hrpmh *h, const char *res, uint32_t *address) {
    uint32_t key[2] = {0, 0};
    for (unsigned i = 0; i < 8 && res[i]; ++i)
        key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
    unsigned walked = 0, best = 0;
    // A layout that already produced a hit is reused for the second rail.
    if (h->sbase) {
        if (hr_walk(h, key, h->sbase, h->hbase, address, &walked)) return 0;
    }
    for (unsigned s = 0; s < 2; ++s) {
        for (unsigned b = 0; b < 2; ++b) {
            if (hr_walk(h, key, hr_sbase[s], hr_hbase[b], address, &walked)) {
                h->sbase = hr_sbase[s];
                h->hbase = hr_hbase[b];
                return 0;
            }
            if (walked > best) best = walked;
        }
    }
    h->diag.walk = best;
    return HR_ERR_CMDDb;
}

int hrpmh_init(struct hrpmh *h, void *rsc, void *cmddb) {
    if (!h) return HR_ERR_PARAM;
    h->rsc = (volatile uint32_t *)rsc;
    h->db = (volatile uint32_t *)cmddb;
    struct hrpmh_diag zero = {0};
    h->diag = zero;
    h->diag.tcs = 0xffffffffu;
    h->sbase = h->hbase = 0;
    if (!h->rsc || !h->db) {h->diag.rc = (uint32_t)HR_ERR_PARAM; return HR_ERR_PARAM;}
    const uint32_t id = hr_rd(h->rsc, 0), config = hr_rd(h->rsc, 0x0c);
    h->major = (id >> 16) & 0xffu;
    h->minor = (id >> 8) & 0xffu;
    h->diag.id = id;
    h->diag.config = config;
    h->diag.version = hr_rd(h->db, 0);
    h->diag.magic = hr_rd(h->db, 4);
    const unsigned ntcs = (config >> (6u * HR_DRV)) & 0x3fu, ncpt = (config >> 27) & 0x1fu;
    if (!(h->major >= 3 || h->major == 2) || ntcs < HR_ACTIVE_N || ntcs > 16 ||
        ncpt < 3 || ncpt > 16) {
        h->diag.rc = (uint32_t)HR_ERR_LAYOUT;
        return HR_ERR_LAYOUT;
    }
    if (h->diag.magic != HR_DB_MAGIC) {
        h->diag.rc = (uint32_t)HR_ERR_CMDDb;
        return HR_ERR_CMDDb;
    }
    return 0;
}

int hrpmh_vote(struct hrpmh *h, const char *resource, uint32_t mV) {
    const struct hr_layout *L = h->major >= 3 ? &hr_v3 : &hr_v2;
    uint32_t address = 0;
    int rc = hr_find(h, resource, &address);
    if (rc) {h->diag.rc = (uint32_t)rc; return rc;}
    // A free ACTIVE TCS: control, enable, wait and the IRQ-status bit all clear, and (the
    // reference driver's empirical check) a real status value. If nothing passes the status
    // test, fall back to the control/enable/wait test alone: those three are what an
    // in-flight TCS actually sets, and `status` is not documented as a free/busy bit -
    // the reference only ever ran against an RSC the bootloader had already used.
    const uint32_t irq_en = hr_rd(h->rsc, HR_TCS_OFF + 0);
    const uint32_t irq_st = hr_rd(h->rsc, HR_TCS_OFF + 4);
    unsigned chosen = 0xffffffffu;
    for (unsigned pass = 0; pass < 2 && chosen == 0xffffffffu; ++pass) {
        for (unsigned t = 0; t < HR_ACTIVE_N; ++t) {
            const uint32_t b = HR_TCS_OFF + t * HR_TCS_STRIDE;
            const uint32_t st = hr_rd(h->rsc, b + L->status), ctl = hr_rd(h->rsc, b + L->control);
            const uint32_t en = hr_rd(h->rsc, b + L->enable), wt = hr_rd(h->rsc, b + L->wait);
            if (ctl != 0 || en != 0 || wt != 0 || (irq_st & (1u << t))) continue;
            if (pass == 0 && (st == 0 || st == 0xffffffffu)) continue;
            chosen = t;
            h->diag.relaxed = pass;
            break;
        }
    }
    if (chosen == 0xffffffffu) {h->diag.rc = (uint32_t)HR_ERR_BUSY; return HR_ERR_BUSY;}
    h->diag.tcs = chosen;
    const uint32_t b = HR_TCS_OFF + chosen * HR_TCS_STRIDE, bit = 1u << chosen;
    if (hr_sync(h, HR_TCS_OFF + 0, irq_en & ~bit)) {h->diag.rc = (uint32_t)HR_ERR_IO; return HR_ERR_IO;}
    // Three commands: voltage at +0, mode at +8 (7 = high power), enable at +4 (1 = on),
    // in that order - rpmh-rsc.c runs the commands in the order they were written.
    static const uint32_t offs[3] = {0u, 8u, 4u};
    uint32_t vals[3] = {mV, 7u, 1u};
    for (unsigned c = 0; c < 3; ++c) {
        const uint32_t cmd = b + c * L->cmd_stride;
        if (hr_sync(h, cmd + L->msgid, HR_WRITE_ACK) || hr_sync(h, cmd + L->addr, address + offs[c]) ||
            hr_sync(h, cmd + L->data, vals[c])) {
            h->diag.rc = (uint32_t)HR_ERR_IO;
            return HR_ERR_IO;
        }
    }
    if (hr_sync(h, b + L->wait, 7) || hr_sync(h, b + L->enable, 7) ||
        hr_sync(h, b + L->control, 0)) {
        h->diag.rc = (uint32_t)HR_ERR_IO;
        return HR_ERR_IO;
    }
    hr_wr(h->rsc, b + L->control, (1u << 16) | (1u << 24));   // trigger
    int done = 0;
    uint32_t cs[3] = {0, 0, 0};
    for (unsigned i = 0; i < 2000 && !done; ++i) {
        hr_wait_us(1000);
        int all = 1;
        for (unsigned c = 0; c < 3; ++c) {
            cs[c] = hr_rd(h->rsc, b + L->cmd_status + c * L->cmd_stride);
            all &= cs[c] != 0xffffffffu && (cs[c] & 0x10100u) == 0x10100u;   // ACK + done
        }
        const uint32_t st = hr_rd(h->rsc, b + L->status);
        h->diag.status = st;
        h->diag.cmd0 = cs[0];
        h->diag.cmd1 = cs[1];
        h->diag.cmd2 = cs[2];
        done = all && st != 0;
    }
    h->diag.done = (uint32_t)done;
    // Put the TCS back exactly as it was found (the vote itself stays latched).
    (void)hr_sync(h, b + L->control, 1u << 16);
    (void)hr_sync(h, b + L->control, 0);
    (void)hr_sync(h, b + L->enable, 0);
    (void)hr_sync(h, b + L->wait, 0);
    hr_wr(h->rsc, HR_TCS_OFF + 8, bit);
    (void)hr_sync(h, HR_TCS_OFF + 0, irq_en);
    h->diag.rc = done ? 0u : (uint32_t)HR_ERR_NOT_DONE;
    return done ? 0 : HR_ERR_NOT_DONE;
}

// Read-only diagnostic: what does the bootloader actually call the LDOs this board uses?
// Linux names them "L<id><pmic>_E<ee>" (rpmh-regulator's "%s%d%s" with pmic-id "B_E0"),
// the legacy form is "ldoB4". We cannot know which one this vendor cmd-db holds, so the
// scan records any key that starts with 'L' and whose decimal digits are 4 or 16 - the two
// rails the touch node names - with its address. Four entries, four lines of panic screen.
void hrpmh_scan(struct hrpmh *h) {
    h->diag.hitN = 0;
    for (unsigned i = 0; i < 8 && h->diag.hitN < 4; ++i) {
        const uint32_t sh = hr_sbase[0] + 16u * i;
        if (!hr_db16(h, sh)) break;
        const uint32_t entries = hr_hbase[1] + hr_db16(h, sh + 2);
        const unsigned count = hr_db16(h, sh + 6);
        if ((entries & 3u) || entries + count * HR_DB_ENTRY > HR_DB_SIZE) continue;
        for (unsigned j = 0; j < count && h->diag.hitN < 4; ++j) {
            const uint32_t e = entries + HR_DB_ENTRY * j;
            const uint32_t k0 = hr_rd(h->db, e);
            if ((k0 & 0xffu) != 'L') continue;
            unsigned value = 0, digits = 0;
            for (unsigned b = 8; b < 32; b += 8) {
                const unsigned c = (k0 >> b) & 0xffu;
                if (c < '0' || c > '9') break;
                value = value * 10u + (c - '0');
                ++digits;
            }
            if (!digits || (value != 4u && value != 16u)) continue;
            const unsigned n = h->diag.hitN++;
            h->diag.hitKey0[n] = k0;
            h->diag.hitKey1[n] = hr_rd(h->db, e + 4);
            h->diag.hitAddr[n] = hr_rd(h->db, e + 16);
        }
    }
}

// Try the handful of names a PM8550B LDO can have in this bootloader's cmd-db, in order.
// Linux builds them as "<type><id><pmic>" with the executive suffix added by the
// bootloader ("L4B_E0" on the reference device), and a vendor cmd-db is free to use the
// bare form instead - hence the short list rather than one guess.
int hrpmh_vote_ldo(struct hrpmh *h, unsigned id, uint32_t mV) {
    char name[12];
    static const char *const suffix[3] = {"_E0", "_E1", ""};
    int last = HR_ERR_CMDDb;
    for (unsigned s = 0; s < 4; ++s) {
        unsigned p = 0;
        if (s < 3) {
            name[p++] = 'L';
            if (id >= 10) name[p++] = (char)('0' + id / 10u);
            name[p++] = (char)('0' + id % 10u);
            name[p++] = 'B';
            const char *q = suffix[s];
            while (*q) name[p++] = *q++;
        } else {
            // rpmh-regulator's legacy single-character pmic-id form: "ldoB4".
            name[p++] = 'l'; name[p++] = 'd'; name[p++] = 'o';
            name[p++] = 'b';
            if (id >= 10) name[p++] = (char)('0' + id / 10u);
            name[p++] = (char)('0' + id % 10u);
        }
        name[p] = 0;
        last = hrpmh_vote(h, name, mV);
        if (!last) return 0;
        if (last != HR_ERR_CMDDb) return last;   // found but the vote itself failed
    }
    return last;
}

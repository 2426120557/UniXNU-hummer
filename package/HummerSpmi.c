// SPDX-License-Identifier: BSD-2-Clause-Patent
// SPMI PMIC-arbiter read path. See HummerSpmi.h for the layout this file implements and
// why the volume key needs it at all.
#include "HummerSpmi.h"

#define HS_CMD 0x00u
#define HS_STATUS 0x08u
#define HS_RDATA0 0x18u

#define HS_OP_EXT_READ 13u
#define HS_OP_READ 15u

#define HS_ST_DONE 0x1u
#define HS_ST_FAILURE 0x2u
#define HS_ST_DENIED 0x4u
#define HS_ST_DROPPED 0x8u

static uint32_t hs_rd(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static void hs_wr(volatile uint8_t *base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

// The arbiter updates STATUS asynchronously; a dsb keeps the compiler and the CPU from
// hoisting the polling loop out of the MMIO window.
static void hs_barrier(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

void hspmi_init(struct hspmi *h, void *core, void *chnls, void *obs)
{
    h->core = (volatile uint8_t *)core;
    h->chnls = (volatile uint8_t *)chnls;
    h->obs = (volatile uint8_t *)obs;
    h->last_apid = 0xffffffffu;
    h->last_map = 0xffffffffu;
    h->last_status = 0xffffffffu;
    h->last_ppid = 0xffffffffu;
    h->map_rc = 0xffffffffu;
    h->err = 0;
    if (!h->core) {
        h->version = 0;
        h->kind = 0;
        return;
    }
    h->version = hs_rd(h->core, 0);
    if (h->version >= 0x70000000u) {
        h->kind = 7;
    } else if (h->version >= 0x50000000u) {
        h->kind = 5;
    } else if (h->version >= 0x20010000u) {
        h->kind = 2;
    } else {
        h->kind = 0;   // v1 uses a single per-EE channel and is not worth carrying here
    }
    if (h->kind == 2) {
        // v2 and older expose the observer channel through the same "core" window.
        h->obs = h->core;
    }
}

// Walk the APID map once per ppid. The tables are at most 1024 words, so this is a
// handful of microseconds - but the key poll runs every 20 ms, so the match is cached:
// re-scanning 8192 bytes 50 times a second would be pure waste.
static int hs_find_apid(struct hspmi *h, uint32_t ppid)
{
    uint32_t base, count, mask;

    if (h->map_rc == 0 && h->last_ppid == ppid && h->last_apid != 0xffffffffu) return 0;

    switch (h->kind) {
    case 7:
        base = 0x2000u; count = 1024u; mask = 0x1fffu;
        break;
    case 5:
        base = 0x0900u; count = 512u; mask = 0x0fffu;
        break;
    case 2:
        base = 0x0800u; count = 512u; mask = 0x0fffu;
        break;
    default:
        return -2;
    }

    h->last_ppid = ppid;
    h->last_map = 0xffffffffu;
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t word = hs_rd(h->core, base + 4u * n);
        if (!word) continue;                       // APID slot is unused
        if (((word >> 8) & mask) == ppid) {
            h->last_apid = n;
            h->last_map = word;
            h->map_rc = 0;
            return 0;
        }
    }
    h->last_apid = 0xffffffffu;
    h->map_rc = -2;
    return -2;
}

static uint32_t hs_obs_offset(const struct hspmi *h, uint32_t apid)
{
    switch (h->kind) {
    case 7: return 0x20u * apid;      // 0x8000 * ee, ee = 0
    case 5: return 0x80u * apid;      // 0x10000 * ee, ee = 0
    default: return 0x8000u * apid;   // 0x1000 * ee, ee = 0
    }
}

int hspmi_read(struct hspmi *h, unsigned sid, unsigned addr, uint8_t *buf, unsigned len)
{
    h->err = 0;
    if (!h->core || !h->obs || !h->kind) {
        h->err = 1;
        return -1;
    }
    if (len < 1 || len > 8 || !buf) {
        h->err = 2;
        return -1;
    }

    const uint32_t ppid = ((uint32_t)sid << 8) | ((uint32_t)(addr >> 8) & 0xffu);
    const int found = hs_find_apid(h, ppid);
    if (found) {
        h->err = 3;
        return found;
    }

    const uint32_t off = hs_obs_offset(h, h->last_apid);
    const uint32_t opc = (len > 1) ? HS_OP_EXT_READ : HS_OP_READ;
    const uint32_t cmd = (opc << 27) | (((uint32_t)addr & 0xffu) << 4) |
                         (((uint32_t)len - 1u) & 0x7u);

    hs_wr(h->obs, off + HS_CMD, cmd);
    hs_barrier();

    uint32_t status = 0;
    int done = 0;
    for (uint32_t spin = 0; spin < 200000u; ++spin) {
        status = hs_rd(h->obs, off + HS_STATUS);
        if (status & HS_ST_DONE) {
            done = 1;
            break;
        }
    }
    h->last_status = status;
    if (!done) {
        h->err = 4;
        return -4;
    }
    if (status & (HS_ST_FAILURE | HS_ST_DENIED | HS_ST_DROPPED)) {
        // 5 = failed, 6 = denied, 7 = dropped
        h->err = (status & HS_ST_FAILURE) ? 5u : ((status & HS_ST_DENIED) ? 6u : 7u);
        return -(int)h->err;
    }

    const uint32_t data = hs_rd(h->obs, off + HS_RDATA0);
    for (unsigned i = 0; i < len && i < 4u; ++i) {
        buf[i] = (uint8_t)(data >> (8u * i));
    }
    return 0;
}

uint32_t hspmi_read8(struct hspmi *h, unsigned sid, unsigned addr)
{
    uint8_t v = 0xff;
    if (hspmi_read(h, sid, addr, &v, 1)) return 0xffffffffu;
    return v;
}

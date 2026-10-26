// SPDX-License-Identifier: BSD-2-Clause-Patent
// SPMI PMIC-arbiter read path - enough to ask the PM8550 what a GPIO is doing.
//
// The volume key on hummer is not a TLMM pad (see TB_SPMI_* in HummerTouchBoard.h): the
// vendor DTS puts it on PM8550 GPIO6, so reading it needs one SPMI register read. There is
// no SPMI driver in this kernel - everything PMIC-shaped so far went through the apps RSC
// (HummerRpmh.c), which cannot read a PMIC register - so this file implements the slice of
// Linux's drivers/spmi/spmi-pmic-arb.c that a single-byte read needs:
//
//   version      core + 0x0000   >= 0x70000000 -> v7, >= 0x50000000 -> v5, else v2
//   APID map     v7: core + 0x2000 + 4*n, n < 1024, entry = (ppid << 8) | owner bits
//                v5: core + 0x0900 + 4*n, n < 512
//                v2: core + 0x0800 + 4*n, n < 512
//   observer     v7: obs + 0x20 * apid        v5: obs + 0x80 * apid
//                v2: obs(core) + 0x8000 * apid        (ee is 0 for apps; no shift)
//   channel      +0x00 CMD, +0x08 STATUS, +0x18 RDATA0
//   cmd          (opc << 27) | ((addr & 0xff) << 4) | (bc & 7), opc 15 = READ, 13 = EXT_READ
//   status       bit0 DONE, bit1 FAILURE, bit2 DENIED, bit3 DROPPED
//
// Only reads are implemented on purpose: nothing here should be able to change a rail.
#ifndef HUMMER_SPMI_H
#define HUMMER_SPMI_H
#include <stdint.h>

struct hspmi {
    volatile uint8_t *core;      // 0xc400000
    volatile uint8_t *chnls;     // 0xc500000 (unused by reads, kept for a later write path)
    volatile uint8_t *obs;       // 0xc440000
    uint32_t version;            // raw PMIC_ARB_VERSION
    uint32_t kind;               // 7 / 5 / 2, 0 = not a layout this file knows
    uint32_t last_apid;          // APID that last matched
    uint32_t last_map;           // raw APID-map word of that match (0xffffffff = no match)
    uint32_t last_status;        // last channel status word
    uint32_t last_ppid;          // ppid the last lookup asked for
    uint32_t map_rc;             // 0 = found, -2 = no APID for that ppid
    uint32_t err;                // 0, or -(3 + status bit) of the failing read
};

// `core`, `chnls` and `obs` are already-mapped kernel virtual addresses.
void hspmi_init(struct hspmi *h, void *core, void *chnls, void *obs);
// Single-byte (or up to 8 byte) read of `addr` on slave `sid`. Returns 0 on success.
int hspmi_read(struct hspmi *h, unsigned sid, unsigned addr, uint8_t *buf, unsigned len);
// Same read, value returned as a word (low byte of the transfer), 0xffffffff on failure.
uint32_t hspmi_read8(struct hspmi *h, unsigned sid, unsigned addr);

#endif

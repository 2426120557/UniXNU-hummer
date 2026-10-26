// SPDX-License-Identifier: BSD-2-Clause-Patent
// apps RSC ("RPMh") DRV-2 vote - the only way to switch the touch panel's rails on.
//
// hummer's touch node has no enable GPIO: sm8750-oneplus-hummer-bringup.dtsi gives
//   vddio-supply = <&vreg_l4b_1p8>;     PM8550B LDO4,  1.8 V
//   avdd-supply  = <&vreg_l16b_2p8>;    PM8550B LDO16, 3.104 V
// i.e. both rails are RPMh regulators, so nothing in the SoC can turn them on by writing
// a TLMM register. Whoever boots this kernel has to place the same vote Linux places:
// one ACTIVE TCS in the apps RSC with voltage / mode / enable commands.
//
// Register layout, cmd-db format and the command sequence come from Linux rpmh-rsc.c
// (ver_2_7 / ver_3_0 layouts) and from the proven XNU S3910 driver on the same SoC
// family (pandora_touch.c). Every write is read back, every command's ACK is polled, and
// the function refuses to touch a TCS whose control/enable/wait are not all zero.
#ifndef HUMMER_RPMH_H
#define HUMMER_RPMH_H
#include <stdint.h>

// Everything the panic screen needs to tell "the rail is up" from "the vote never ran".
struct hrpmh_diag {
    uint32_t id;        // RSC_HW_VERSION: major = bits 23:16, minor = 15:8
    uint32_t config;    // RSC_HW_CONFIG
    uint32_t version;   // cmd-db version word
    uint32_t magic;     // cmd-db magic (0x0c0330db)
    uint32_t address;   // VRM address cmd-db returned for the last resource
    uint32_t found;     // how many matching VRM entries the last lookup produced
    uint32_t sbase;     // slave-table base that produced a match (0 = none yet)
    uint32_t hbase;     // cmd-db data base that produced a match (0 = none yet)
    uint32_t walk;      // entries walked in the best attempt (0 = the walk never started)
    uint32_t k0, k1;    // first entry's 8-byte resource key (should read as ASCII)
    uint32_t hitN;      // entries whose key looks like L4*/L16* (see hrpmh_scan)
    uint32_t hitKey0[4], hitKey1[4], hitAddr[4];
    uint32_t tcs;       // ACTIVE TCS used, 0xffffffff = none found idle
    uint32_t relaxed;   // 1 = the strict status check found nothing, control/enable/wait did
    uint32_t done;      // 1 = all three commands ACKed and the TCS reported completion
    uint32_t status;    // raw TCS status of the last vote
    uint32_t cmd0, cmd1, cmd2;
    uint32_t rc;        // last vote result (0 = the rail is up)
};

struct hrpmh {
    volatile uint32_t *rsc;     // drv-2 register window (0x16520000)
    volatile uint32_t *db;      // aop_cmd_db_region (0x81c60000)
    uint32_t major, minor;
    unsigned sbase, hbase;      // cmd-db layout discovered by the first successful lookup
    struct hrpmh_diag diag;
};

// Returns 0 when the RSC and cmd-db look like the layouts this file implements.
int hrpmh_init(struct hrpmh *h, void *rsc, void *cmddb);
// Vote `resource` (exact cmd-db key, e.g. "L4B_E0") to `mV` in high-power mode and enable
// it. The TCS is released afterwards, exactly like the reference driver: the vote stays
// latched in the RSC, the TCS goes back to the pool.
int hrpmh_vote(struct hrpmh *h, const char *resource, uint32_t mV);
// Convenience for the two rails this board needs: tries "<res>_E0", "<res>_E1" and the
// bare name, so a vendor cmd-db that names them differently still gets its vote.
int hrpmh_vote_ldo(struct hrpmh *h, unsigned id, uint32_t mV);
// Read-only pass over the documented cmd-db layout that records up to four entries whose
// key starts with 'L' and a decimal 4 or 16 - i.e. the LDO this board's touch node names.
// The keys go to the panic screen, so a boot where the vote still fails says what the
// bootloader actually called those rails.
void hrpmh_scan(struct hrpmh *h);

#endif

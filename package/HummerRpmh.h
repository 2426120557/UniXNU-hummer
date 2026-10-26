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
// family (pandora_touch.c: apt_rpmh_vote). Nothing is guessed: every write is read back,
// every command's ACK is polled, and the function bails out before touching anything if
// the RSC reports a layout or a TCS state it does not recognise.
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
    uint32_t found;     // how many matching VRM entries that lookup produced
    uint32_t tcs;       // ACTIVE TCS used, 0xffffffff = none found idle
    uint32_t done;      // 1 = all three commands ACKed and the TCS reported completion
    uint32_t status;    // raw TCS status of the last vote
    uint32_t cmd0, cmd1, cmd2;
    uint32_t rc;        // last vote result (0 = the rail is up)
};

struct hrpmh {
    volatile uint32_t *rsc;     // drv-2 register window (0x16520000)
    volatile uint32_t *db;      // aop_cmd_db_region (0x81c60000)
    uint32_t major, minor;
    struct hrpmh_diag diag;
};

// Returns 0 when the RSC and cmd-db look like the layouts this file implements.
int hrpmh_init(struct hrpmh *h, void *rsc, void *cmddb);
// Vote `resource` (cmd-db name, e.g. "L4B_E0") to `mV` in high-power mode and enable it.
// The TCS is released afterwards, exactly like the reference driver: the vote stays
// latched in the RSC, the TCS goes back to the pool.
int hrpmh_vote(struct hrpmh *h, const char *resource, uint32_t mV);

#endif

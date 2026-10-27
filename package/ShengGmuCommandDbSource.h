/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * SM8750 (hummer) AOP command-database reservation.
 *
 * Source: the hummer device tree reserved-memory node
 *
 *   aop_cmd_db_region@81c60000 {
 *       compatible = "qcom,cmd-db";
 *       no-map;
 *       reg = <0x00 0x81c60000 0x00 0x20000>;
 *   };
 *
 * so the database lives at physical 0x81c60000 and the reservation is exactly
 * 0x20000 bytes.  ShengGmuCommandDbInputs() requires the size to match the
 * reservation byte for byte and refuses anything else, so these two values are
 * the whole contract.
 *
 * The blob itself is AOP-owned firmware data, not ours: it is read at boot from
 * the reserved region and never written.  Only the version word is a caller
 * input (sheng-gpu-cmddb-version), because a version value read back from the
 * same blob cannot qualify it.
 */
#ifndef SHENG_GMU_COMMAND_DB_SOURCE_H
#define SHENG_GMU_COMMAND_DB_SOURCE_H
#include <stdint.h>
#include <stddef.h>

#define SHENG_GMU_COMMAND_DB_BASE  0x81c60000ull
#define SHENG_GMU_COMMAND_DB_BYTES 0x20000u
#endif

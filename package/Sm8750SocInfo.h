/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Read only headers and socinfo from the 0x81d00000/2 MiB SMEM mapping;
 * other partitions may be inaccessible. Supports version 12 global partitions.
 * Board must check the bootloader-owned socinfo item is stable.
 * No SMEM allocation, remote lock or hardware write. */
int Sm8750SocInfoGpuCode(const volatile uint8_t *,size_t,uint32_t *soc_id,uint32_t *soc_code);
#ifdef __cplusplus
}
#endif

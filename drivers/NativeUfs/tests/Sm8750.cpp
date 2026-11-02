// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include "../GccUfs.h"
int main() {
  static_assert(UFS_HC_BASE==0x1d84000 && UFS_HC_SIZE==0x3000);
  static_assert(GCC_UFS_PHY_AHB_ENABLE==0x77028 && GCC_UFS_PHY_UNIPRO_CORE_ENABLE==0x7706c);
  static_assert(GCC_UFS_PHY_ICE_CORE_ENABLE==0x7707c && GCC_AGGRE_UFS_PHY_AXI_ENABLE==0x770f0);
  assert(ufsBusClocksOn(0x80000000,1,1,1));
  assert(ufsBusClocksOn(0x80000000,0x80000003,0x80000003,0x80000003));
  assert(!ufsBusClocksOn(0,1,1,1));
  assert(!ufsBusClocksOn(0x80000000,0,1,1));
  assert(!ufsBusClocksOn(0x80000000,1,0x80000001,1));
  assert(!ufsBusClocksOn(0x80000000,1,1,0));
  puts("PASS: SM8750 clock offsets and power/clock admission");
}

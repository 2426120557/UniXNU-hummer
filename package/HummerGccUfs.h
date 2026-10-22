// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8750 (hummer / OnePlus Ace 5 Pro) GCC register offsets used by the UFS probe.
//
// Why this file exists: the NativeUfs submodule ships GccUfs.h transcribed from
// Linux gcc-sm8550.c, and the two SoCs do NOT share the UFS PHY branch layout.
// On SM8750 every UFS PHY branch register after the AXI branch sits 4 bytes
// higher; the SM8550 AHB offset 0x77024 is a reserved word there.  Reading it
// returned 0, so NativeUfs::inspect() concluded the AHB branch was off and
// skipped the host-controller read entirely (first hummer run: ahb=00000000,
// hc_read=0, while the neighbours it *did* read were healthy: axi=1 at 0x77018,
// 0x77028 = 3 i.e. the real AHB CBCR enabled with hwcg, 0x7702c = 1 i.e. the real
// TX symbol CBCR, GDSCR 0xfa2f7600 with PWR_ON set).
//
// Provenance: every value below is transcribed from
//   drivers/clk/qcom/gcc-sm8750.c in Linus' tree
//   (gcc_ufs_phy_ahb_clk .halt_reg/.hwcg_reg, gcc_ufs_mem_phy_gdsc .gdscr,
//    gcc_ufs_phy_gdsc .gdscr, GCC_UFS_PHY_BCR).
// The GCC block itself is clock-controller@100000 (reg size 0x1f4200) and the
// host controller is ufshc@1d84000 (0x3000) in sm8750.dtsi, same bases as SM8550.
//
// This header is force-included (`-include`) ahead of GccUfs.h and defines that
// file's include guard, so its SM8550 body is skipped without touching the
// submodule.  build-arm-driver.py asserts the guard still exists.

#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// Physical windows: sheng used the same two bases; both are confirmed by the
// hummer DTB (clock-controller@100000, ufshc@1d84000).
#define GCC_BASE                      0x00100000ULL
#define UFS_HC_BASE                   0x01d84000ULL
#define UFS_HC_SIZE                   0x3000ULL

// GDSCs.  Same offsets on SM8550 and SM8750.
// GDSCR bit31 = PWR_ON, bit30 = SW_OVERRIDE, bit29 = HW_CONTROL, bit28 = SW_COLLAPSE,
// bits 23:20 EN_REST_WAIT, 19:16 EN_FEW_WAIT, 15:12 CLK_DIS_WAIT.
#define GCC_UFS_PHY_GDSCR             0x77004
#define GCC_UFS_MEM_PHY_GDSCR         0x9e000

// gcc_ufs_phy_axi_clk: same offset on both SoCs.
#define GCC_UFS_PHY_AXI_ENABLE        0x77018
// gcc_ufs_phy_ahb_clk: SM8550 0x77024 -> SM8750 0x77028.
#define GCC_UFS_PHY_AHB_ENABLE        0x77028
// gcc_ufs_phy_tx_symbol_0_clk: SM8550 0x77028 -> SM8750 0x7702c.
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x7702c
// gcc_ufs_phy_rx_symbol_0_clk: SM8550 0x7702c -> SM8750 0x77030.
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x77030
// gcc_ufs_phy_unipro_core_clk: SM8550 0x77068 -> SM8750 0x7706c.
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x7706c
// gcc_ufs_phy_ice_core_clk: SM8550 0x77074 -> SM8750 0x7707c.
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x7707c
// gcc_ufs_phy_phy_aux_clk: SM8550 0x770b0 -> SM8750 0x770bc.
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x770bc
// gcc_ufs_phy_rx_symbol_1_clk: SM8550 0x770cc -> SM8750 0x770d8.
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x770d8
// gcc_aggre_ufs_phy_axi_clk: SM8550 0x770e4 -> SM8750 0x770f0; hwcg_bit 1.
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x770f0

// CBCR bit0 is CLK_ENABLE for every branch above; several set hwcg_bit=1, so
// bit31 CLK_OFF may legitimately read 1 on an idle branch while bit0 stays set.
#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U

// Not represented, exactly as in the submodule: reset (GCC_UFS_PHY_BCR 0x77000),
// RPMh votes and TCSR/QMP PHY programming.  NativeUfs must not write them
// speculatively; the probe is read-only.

#endif

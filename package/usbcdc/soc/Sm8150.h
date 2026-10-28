/* SPDX-License-Identifier: MIT
 * SM8150 (Xiaomi nabu) GCC + APPS SMMU + DWC3 profile.
 *
 * Every value below has a source that was checked on 2026-10-26:
 *   DT            /root/dwc3/UniXNU-nabu/dts/nabu-full.dts (ssusb@a600000, :28689-28800)
 *   clock driver  mainline drivers/clk/qcom/gcc-sm8150.c
 *   clock ids     mainline include/dt-bindings/clock/qcom,gcc-sm8150.h
 *   hardware      /root/archive/dwc3-old-20261026-1725/NativeDwc3/GccUsb.h (same offsets,
 *                 already exercised on this board)
 *
 * The DT describes a high-speed-only port (maximum-speed = "high-speed") and leaves the
 * QMP combo PHY disabled, so only the USB2 branch and the femto HS PHY matter here.
 */
#ifndef QUD_SM8150_H
#define QUD_SM8150_H

#define QUD_SOC_NAME "sm8150-nabu"

/* Development IDs inherited from m1n1, same as the SM8550 profile. */
#define QUD_USB_VID 0x1209u
#define QUD_USB_PID 0x316du
#define QUD_USB_SERIAL "QcomXnuUsbDebug"
#define QUD_USB_PRODUCT "Qualcomm XNU Debug"

/* Board facts the module cannot verify for us (measured on nabu, 2026-10-25/26):
 *  - the GCC resets do not self-clear: write 1 (assert) and then 0 (deassert);
 *  - ABL hands the core over in HOST mode and halted, so clocks/GDSC/PHY and the
 *    device-mode soft reset must run before this module is allowed to start;
 *  - the HS PHY is the femto block at 0x088E2000 (0x110 window), driven by the
 *    vendor phy-msm-snps-hs sequence, not by qusb2 offsets.
 */
#define QUD_GCC_BASE 0x00100000ULL
#define QUD_GCC_BYTES 0x100000u   /* 0x8c008/0x8c028 clkref sit above the 0x40000 window */
#define QUD_DWC3_BASE 0x0a600000ULL          /* DT reg = <0xa600000 0x100000> */
#define QUD_DWC3_BYTES 0x100000u
#define QUD_SMMU_BASE 0x15000000ULL          /* APPS SMMU */
#define QUD_SMMU_BYTES 0x100000ULL

/* HS PHY: hsphy@88e2000, qcom,usb-hsphy-snps-femto (not qusb2). DT reg window. */
#define QUD_PHY_BASE 0x088E2000ULL
#define QUD_PHY_BYTES 0x110u

/* DT iommus = <&apps_smmu 0x140 0x0> on ssusb@a600000. */
#define QUD_STREAM_ID 0x140u

/* XBL/ABL already leaves a valid SMR for this SID: the old line read
 * SMR=0x80000140 and S2CR=3 (type 0, translated), so the private-domain
 * install is refused with StreamExists (board 2026-10-26, dr=5). The module's
 * identity lease takes that exact pair over as TYPE-only bypass, saves and
 * restores it, and DMA then uses the static buffer's physical address. */
#define QUD_DMA_PREFER_IDENTITY 1

/* This firmware rejects a TYPE=BYPASS S2CR: r4 wrote 0x00010003 on the
 * inherited entry and read back 0x00020003 (TYPE=BYPASS became TYPE=FAULT,
 * CBNDX preserved). The board's own UFS takeover and the sheng port both use a
 * bypass context bank instead (SCTLR=0, CBAR=0x10000 CBA2R=1) selected by a
 * TYPE=TRANSLATE S2CR, and note that SM8150 does not read back written S2CR/
 * CBAR/SCTLR values. Route USB through soc/QcomUsbBypass.c. */
#define QUD_DMA_BYPASS_BANK 1

/* Private translated-domain IOVA: free choice, 2 MiB aligned, below 4 GiB. */
#define QUD_DMA_IOVA 0x90000000ULL

/* gcc-sm8150.c clk_branch cbcr offsets: master, sleep, mock_utmi, phy_aux, phy_com_aux.
 * (pipe_clk 0xf058 is HALT_SKIP and is left to the PHY owner.) */
#define QUD_GCC_CBCR_OFFSETS {0x0f010u, 0x0f014u, 0x0f018u, 0x0f050u, 0x0f054u}
/* Device-mode bring-up enables the AXI/pipe branches and both USB3 clkrefs as well:
 * without a clkref the femto PHY PLL has no reference (board record 2026-10-24). */
#define QUD_GCC_CLOCK_OFFSETS {0x0f010u, 0x0f014u, 0x0f018u, 0x0f050u, 0x0f054u, \
                                0x0f058u, 0x0f078u, 0x0f07cu, 0x8c008u, 0x8c028u}
/* pipe_clk has HALT_SKIP semantics: enable it but do not poll CLK_OFF. */
#define QUD_GCC_CLOCK_NO_POLL_OFFSETS {0x0f058u, 0x8c008u, 0x8c028u}
#define QUD_GCC_QUSB2PHY_BCR_OFFSET 0x12000u
#define QUD_GCC_AHB2PHY_BCR_OFFSET 0x6a000u
#define QUD_GCC_BCR_OFFSET 0x0f000u          /* GCC_USB30_PRIM_BCR, DT reset id 26 */
#define QUD_GCC_GDSCR_OFFSET 0x0f004u        /* usb30_prim_gdsc, PWR_ON = BIT(31) */
#define QUD_GCC_CFG_GDSCR_OFFSET 0x0f008u    /* gdscr + 4; unused while QUD_GDSC_LEGACY_PWR_ON */

/* The nabu gcc node has no qcom,support-cfg-gdscr: the GDSC reports through
 * GDSCR.PWR_ON (bit31), polled after clearing SW_COLLAPSE (bit0). */
#define QUD_GDSC_LEGACY_PWR_ON 1

/* QSCRATCH inside the dwc3-msm wrapper: 0x000F8800 + 0x10 on SM8150 as well. */
#define QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0x0f8810u
/* SM8150 dwc3-msm only drives UTMI_OTG_VBUS_VALID; BIT(28) is SM8550-specific. */
#define QUD_QSCRATCH_VBUS_MASK (1u << 20)

/* nabu_psci_reset() already issues SMC 0x84000009 on this board. */
#define QUD_PSCI_SMC_RESET 1

#endif

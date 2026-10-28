/* SPDX-License-Identifier: MIT
 * SM8750 (OnePlus Ace 5 Pro, hummer) GCC + APPS SMMU + DWC3 profile.
 *
 * Every value below has a source that was checked on 2026-10-28 against the
 * mainline SM8750 support already present in the board's own reference tree
 * (linux_oneplus_ace5_pro, v7.2.9):
 *   DT          arch/arm64/boot/dts/qcom/sm8750.dtsi
 *                 usb@a600000          (reg, clocks, iommus, resets, power-domain)
 *                 phy@88e3000          usb_hsphy, qcom,sm8750-m31-eusb2-phy
 *                 phy@88e8000          usb_dp_qmpphy (USB3/DP combo, unused here)
 *                 iommu@15000000       apps_smmu, qcom,sm8750-smmu-500
 *   clock drv   drivers/clk/qcom/gcc-sm8750.c   (cbcr offsets, GDSC, resets)
 *   dwc3 glue   drivers/usb/dwc3/dwc3-qcom.c    (QSCRATCH at 0xf8800 + 0x10)
 *
 * What is already established for hummer and what is not:
 *   measured   the device is in fastboot when XNU starts, so ABL has brought
 *              DWC3 up in DEVICE mode with the USB2 PHY running -- the module's
 *              inherited-state path is the one to try first;
 *   unverified whether this firmware leaves an SMR/S2CR pair for SID 0x40, and
 *              whether it rejects S2CR.TYPE=BYPASS the way the SM8150 firmware
 *              did. The first boot is expected to answer both (dr/ds/bank).
 *   unverified the M31 eUSB2 PHY sequence: QcomUsbPrepare implements the SM8150
 *              femto block at 0x88e2000, while SM8750 has the M31 eUSB2 block at
 *              0x88e3000. QUD_PHY_BASE is therefore left undefined and
 *              qcom-usb-prepare=1 must NOT be used until that sequence exists.
 */
#ifndef QUD_SM8750_H
#define QUD_SM8750_H

#define QUD_SOC_NAME "sm8750-hummer"

/* Development IDs inherited from m1n1, same as the other profiles. */
#define QUD_USB_VID 0x1209u
#define QUD_USB_PID 0x316du
#define QUD_USB_SERIAL "QcomXnuUsbDebug"
#define QUD_USB_PRODUCT "Qualcomm XNU Debug"

#define QUD_GCC_BASE 0x00100000ULL
/* USB's blocks end at 0x39090 (aggre AXI branch); the QUSB2 PHY BCR is at
 * 0x12000 and there is no AHB2PHY reset in the SM8750 DT, so 0x40000 is enough
 * for every offset used here. */
#define QUD_GCC_BYTES 0x40000u

#define QUD_DWC3_BASE 0x0a600000ULL   /* DT reg = <0x0a600000 0xfc100> */
#define QUD_DWC3_BYTES 0x100000u
#define QUD_SMMU_BASE 0x15000000ULL   /* apps_smmu, DT reg = <0x15000000 0x100000> */
#define QUD_SMMU_BYTES 0x100000ULL

/* DT iommus = <&apps_smmu 0x40 0x0> on usb@a600000. (Same SID as SM8550.) */
#define QUD_STREAM_ID 0x40u

/* Whether ABL pre-routed this SID is exactly what the first boot reports. If it
 * did not, the private translated domain installs instead and this stays 0. */
#define QUD_DMA_PREFER_IDENTITY 0

/* Same Qualcomm SMMUv500 firmware family as SM8150/SM8550: if it refuses
 * S2CR.TYPE=BYPASS (reads back as FAULT), the bypass context bank route is the
 * one this board's UFS takeover already uses. Switch this on only after a boot
 * shows the identity lease being refused. */
#define QUD_DMA_BYPASS_BANK 0

/* Private translated-domain IOVA: free choice, 2 MiB aligned, below 4 GiB. */
#define QUD_DMA_IOVA 0x90000000ULL

/* gcc-sm8750.c clk_branch halt/cbcr offsets for the five DT clocks:
 *   master 0x39018, sleep 0x39028, mock_utmi 0x3902c,
 *   cfg_noc AXI 0x3908c, aggre AXI 0x39090.
 * (The pipe branch 0x3906c is HALT_DELAY and belongs to the PHY owner.) */
#define QUD_GCC_CBCR_OFFSETS {0x39018u, 0x39028u, 0x3902cu, 0x3908cu, 0x39090u}

/* Device-mode bring-up would also need the PHY branches (aux 0x39064,
 * com_aux 0x39068, pipe 0x3906c) and the two TCSR clkrefs -- but the SM8750
 * clkrefs live in tcsrcc, not GCC, so they cannot be expressed here yet. Not
 * used while QUD_PHY_BASE is undefined. */
#define QUD_GCC_CLOCK_OFFSETS {0x39018u, 0x39028u, 0x3902cu, 0x3908cu, 0x39090u, \
                               0x39064u, 0x39068u, 0x3906c}
#define QUD_GCC_CLOCK_NO_POLL_OFFSETS {0x3906c}
#define QUD_GCC_QUSB2PHY_BCR_OFFSET 0x12000u   /* GCC_QUSB2PHY_PRIM_BCR */
#define QUD_GCC_AHB2PHY_BCR_OFFSET 0x12000u    /* not present on SM8750; same block */
#define QUD_GCC_BCR_OFFSET 0x39000u            /* GCC_USB30_PRIM_BCR */
#define QUD_GCC_GDSCR_OFFSET 0x39004u          /* gcc_usb30_prim_gdsc */
#define QUD_GCC_CFG_GDSCR_OFFSET 0x39008u      /* gdscr + 4; POLL_CFG_GDSCR set in Linux */

/* gcc-sm8750.c marks gcc_usb30_prim_gdsc with POLL_CFG_GDSCR, so the power
 * state is CFG_GDSCR.POWER_UP_COMPLETE (bit16), not GDSCR.PWR_ON (bit31). */
#define QUD_GDSC_LEGACY_PWR_ON 0

/* QSCRATCH inside the dwc3 wrapper: the Linux glue maps
 * dwc3_base + 0xf8800 for all of SDM845..SM8750, and HS_PHY_CTRL sits at +0x10. */
#define QUD_QSCRATCH_HS_PHY_CTRL_OFFSET 0xf8810u
/* dwc3-qcom sets UTMI_OTG_VBUS_VALID (bit20); bit28 is SM8550-specific. */
#define QUD_QSCRATCH_VBUS_MASK (1u << 20)

/* No PSCI SYSTEM_RESET through SMC has been qualified on hummer. */
#define QUD_PSCI_SMC_RESET 0

#endif

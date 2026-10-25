// SPDX-License-Identifier: BSD-2-Clause-Patent
// hummer (SM8750) touch board facts and the pure helpers around them. Every
// constant states its source. Nothing here touches hardware; HummerTouchProbe.cpp
// owns the MMIO. Sources: AnyLaySys/linux_oneplus_ace5_pro (DeviceTree + drivers).
#ifndef HUMMER_TOUCH_BOARD_H
#define HUMMER_TOUCH_BOARD_H
#include <stdint.h>

// sm8750.dtsi: spi4 = wrapper geniqup@ac0000, engine reg 0xa90000 size 0x4000.
#define TB_QUP_WRAPPER_BASE 0x00ac0000ULL
#define TB_SE_BASE 0x00a90000ULL
#define TB_SE_SIZE 0x4000ULL
// sm8750.dtsi spi4 has NO iommus property (SM8550's spi4 carried apps_smmu 0xa3):
// the engine's DMA master addresses physical memory directly, so a DMA buffer
// needs no SMMU programming. apps_smmu itself sits at 0x15000000 (iommu@15000000).
#define TB_APPS_SMMU_BASE 0x15000000ULL
#define TB_APPS_SMMU_SIZE 0x100000ULL

// gcc-sm8750.c / sm8750.dtsi clock-controller@100000 (reg size 0x1f4200).
#define TB_GCC_BASE 0x00100000ULL
// Every QUPv3 wrap1 branch below votes through this register (gcc-sm8750.c enable_reg).
#define TB_GCC_VOTE_ENABLE 0x52008
// gcc-sm8750.c branch structs: the vote bits match SM8550 exactly, the halt regs moved.
#define TB_VOTE_WRAP1_CORE_2X (1u<<18)   // gcc_qupv3_wrap1_core_2x_clk, halt 0x2315c
#define TB_VOTE_WRAP1_CORE (1u<<19)      // gcc_qupv3_wrap1_core_clk,   halt 0x23148
#define TB_VOTE_WRAP1_M_AHB (1u<<20)     // gcc_qupv3_wrap_1_m_ahb_clk, halt 0x23140
#define TB_VOTE_WRAP1_S_AHB (1u<<21)     // gcc_qupv3_wrap_1_s_ahb_clk, halt 0x23144
#define TB_VOTE_WRAP1_S4 (1u<<26)        // gcc_qupv3_wrap1_s4_clk,     halt 0x183cc
#define TB_VOTE_ALL (TB_VOTE_WRAP1_CORE_2X|TB_VOTE_WRAP1_CORE|TB_VOTE_WRAP1_M_AHB|TB_VOTE_WRAP1_S_AHB|TB_VOTE_WRAP1_S4)
#define TB_HALT_CORE_2X 0x2315c
#define TB_HALT_CORE 0x23148
#define TB_HALT_M_AHB 0x23140
#define TB_HALT_S_AHB 0x23144
#define TB_HALT_S4 0x183cc
#define TB_CBCR_CLK_OFF (1u<<31)
// SM8750 gpll0: enable_reg 0x52020 BIT(0) (gcc_gpll0). SM8550 used 0x52018 - do not copy.
#define TB_GCC_GPLL0_ENABLE 0x52020
#define TB_GCC_GPLL0_MODE 0x0
// gcc_qupv3_wrap1_s4_clk_src: cmd_rcgr 0x183dc, mnd_width 16, hid_width 5, DFS-managed
// (clk-rcg2.c layout, same sub-offsets as the SM8550 RCG sheng used).
#define TB_S4_RCG 0x183dc
#define TB_RCG_CMD 0x0
#define TB_RCG_CFG 0x4
#define TB_RCG_DFSR_CMD 0x14                 // bit0 DFS enable, bits 4:1 current level
#define TB_RCG_PERF_CFG(l) (0x1c+4*(l))
#define TB_RCG_PERF_M(l) (0x5c+4*(l))
#define TB_RCG_PERF_N(l) (0x9c+4*(l))
#define TB_DFS_LEVELS 8
// gcc_parent_map_0 (the map every QUP SE clock source uses): 0 = TCXO, 1 = GPLL0 main,
// 6 = GPLL0 even. GPLL0 main is 600 MHz and even 300 MHz, inferred from the frequency
// table (100 MHz = main/6, 75 MHz = even/4) - same reasoning as sheng's TouchBoard.h.
static inline uint64_t tb_src_hz(unsigned sel) {return sel==0?19200000ULL:sel==1?600000000ULL:sel==6?300000000ULL:0;}

// TLMM (sm8750.dtsi pinctrl@f100000, reg size 0x102000; pinctrl-sm8750.c PINGROUP:
// ctl_reg = 0x1000*gpio, io_reg = +4, mux_bit 2, pull_bit 0, drv_bit 6, oe_bit 9,
// out_bit 1, in_bit 0; intr: enable 0, polarity 1, detection 3:2, raw status 4,
// target 8 with kpss val 3, status +0xc).
#define TB_TLMM_BASE 0x0f100000ULL
#define TB_GPIO_STRIDE 0x1000
// sm8750.dtsi qup-spi4-data-clk: "MISO, MOSI, CLK" = gpio48/49/50; qup-spi4-cs: gpio51.
#define TB_GPIO_SPI_MISO 48
#define TB_GPIO_SPI_MOSI 49
#define TB_GPIO_SPI_CLK 50
#define TB_GPIO_SPI_CS 51
#define TB_GPIO_POWER 108                      // panel power/AVDD enable (tycykp giulia pin[4], driven high)
#define TB_GPIO_RESET 161                    // reset-gpios = <&tlmm 161 GPIO_ACTIVE_LOW>
#define TB_GPIO_IRQ 162                      // interrupts = <162 IRQ_TYPE_LEVEL_LOW>; bring-up polls instead
// pinctrl-msm.c writes the GROUP-LOCAL function index (position in the PINGROUP funcs
// array), not the global pinctrl enum value, and pinctrl-msm.h hard-codes msm_mux_gpio
// at funcs[0]. The target kernel's own table (drivers/pinctrl/qcom/pinctrl-sm8750.c in
// AnyLaySys/linux_oneplus_ace5_pro) reads
//   [48] = PINGROUP(48, qup1_se4, ibi_i3c, sdc4_cmd, ...)
//   [49] = PINGROUP(49, qup1_se4, ibi_i3c, sdc40, ...)
//   [50] = PINGROUP(50, qup1_se4, aoss_cti, sdc4_clk, ...)
//   [51] = PINGROUP(51, qup1_se4, aoss_cti, sdc41, ...)
// i.e. every one of the four pins selects the SAME function, index 1. The vendor DTS
// names them qup1_se4_l0..l3, but those are lane names inside that port's own pinctrl
// table, not four different selector values. (The global enum index 92 is a trap: it
// never reaches HW.)
#define TB_FUNC_QUP1_SE4 1
#define TB_DRIVE_6MA 2                       // drive-strength 6: (mA/2)-1 encoded
#define TB_DRIVE_2MA 0                       // drive-strength 2 (reset pin, bringup dtsi)
#define TB_OE_BIT 9
#define TB_OUT_BIT 1
#define TB_IN_BIT 0
// bringup dtsi touchscreen@0: spi-max-frequency 19000000, no spi-cpha/spi-cpol -> mode 0.
#define TB_SPI_MODE 0
#define TB_SPI_DEFAULT_HZ 19000000u
#define TB_SPI_MAX_HZ 19000000u

// NativeHidInput's digitizer normalises X by 30480 and Y by 20320 (10x the sheng panel
// geometry): publish in that space so no HID-side change is needed.
#define TB_UI_X_MAX 30479
#define TB_UI_Y_MAX 20319

static inline uint32_t tb_pin_cfg(unsigned func, unsigned drive_code) {return (func&15u)<<2|(drive_code&7u)<<6;}   // pull 0 = none
static inline uint32_t tb_pin_cfg_out(unsigned drive_code) {return tb_pin_cfg(0, drive_code) | (1u<<TB_OE_BIT);}   // gpio func + output enable
static inline uint32_t tb_tlmm_ctl(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE;}
static inline uint32_t tb_tlmm_io(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE+4;}
static inline uint32_t tb_gpio_out_io(int high) {return high ? (1u<<TB_OUT_BIT) : 0;}   // bit1 = output value

// clk-rcg2.c calc_rate: half-integer divider, then M/N.
static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) {prate*=2;prate/=hid+1;}
    if (mode && n) {prate*=m;prate/=n;}
    return prate;
}
// Frequency of DFS level l from its CFG (src 10:8, mode 13:12, hid 4:0 on this 5-bit RCG),
// M register and N register (stored inverted: n = (~N & mask) + m, mnd_width 16).
static inline uint64_t tb_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src=(cfg>>8)&7,mode=(cfg>>12)&3,hid=cfg&0x1f;
    uint32_t m=0,n=0;
    if (mode) {m=mreg&0xffff;n=((~nreg)&0xffff)+m;}
    return tb_calc_rate(tb_src_hz(src),m,n,mode,hid);
}
struct tb_clock_choice {unsigned level, div;uint64_t sclk_hz, actual_hz;};
// Choose the DFS level that gives the highest SPI rate not above target with divider
// div = ceil(sclk / (os * target)). Returns 0 on success.
static inline int tb_choose_clock(const uint64_t *level_hz, unsigned n, unsigned os, unsigned target, struct tb_clock_choice *out) {
    int found=0;
    for (unsigned l=0;l<n;++l) {
        uint64_t f=level_hz[l];if (!f) continue;
        uint64_t d=((f+(uint64_t)os*target-1)/((uint64_t)os*target));if (d<1) d=1;if (d>0xfff) continue;
        uint64_t actual=f/(os*d);if (actual>target) continue;
        if (!found || actual>out->actual_hz || (actual==out->actual_hz && d<out->div)) {
            out->level=l;out->div=(unsigned)d;out->sclk_hz=f;out->actual_hz=actual;found=1;
        }
    }
    return found?0:-1;
}
#endif

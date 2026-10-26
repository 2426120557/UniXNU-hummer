// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm GSI/GPI DMA engine for hummer's spi4 touch controller. See HummerGsi.h.
//
// Ported from the mainline flow (no code copied):
//   drivers/dma/qcom/gpi.c            - GPII register map, channel/event rings, TREs
//   drivers/soc/qcom/qcom-geni-se.c   - geni_se_select_gpi_mode()
//   drivers/spi/spi-geni-qcom.c       - setup_gsi_xfer() / gpi_create_spi_tre()
//
// Differences from Linux, both forced by this bring-up:
//   * polling instead of the GIC (no handler is installed for 279..284 / 293..298),
//     so every GPI interrupt source is masked and progress is read from the context
//     and event-ring registers;
//   * the transfer completion is detected directly in the event ring instead of
//     through a dmaengine callback.
#include "HummerGsi.h"

// GPII register offsets are relative to the engine's "ee_base", which mainline derives
// as regs - ee_offset: 0x10000 for the sm6350-compatible block, 0x0 for
// sdm845/sm8150/sm8250. This part is 0x10000, and the hardware says so: with that
// reading the four implemented instances sit at 0xa10000..0xa24000 and every access
// works, while reading the same GPII offsets as ee_offset 0 (0xa27088 for GPII 5) takes a
// synchronous external abort - the block simply stops decoding above 0xa24xxx, which is
// exactly the DT's dma-channel-mask = 0x1e (instances 1..4). Do not "probe" the other
// reading again: it panics the kernel on the first write.
#define HGS_EE_OFF 0x10000u

// Channel context block (GPII_n_CH_k_*), n = GPII instance, k = channel 0..1.
#define HGS_CH_CNTXT(n, k) (0x20000u + 0x4000u * (n) + 0x80u * (k))
#define HGS_CH_QOS(n, k) (0x2005Cu + 0x4000u * (n) + 0x80u * (k))
#define HGS_CH_SCRATCH(n, k) (0x20060u + 0x4000u * (n) + 0x80u * (k))
#define HGS_CH_DB(n, k) (0x22000u + 0x4000u * (n) + 0x8u * (k))
#define HGS_CH_CMD(n) (0x23008u + 0x4000u * (n))
// EE-level block. The IPA driver's register table (gsihal_reg.c) puts GSI_EE_n_GSI_STATUS at
// 0x25000 with CH_CMD at 0x25008, EV_CMD 0x25010, TYPE_IRQ 0x25080 - the same sub-offsets the
// GPI block uses at 0x23000. So the GPI's EE status/version registers are:
//   GSI_STATUS 0x23000 (bit0 = ENABLED), CH_CMD 0x23008, SW_VERSION 0x23044,
//   HW_PARAM_2 0x23040, HW_PARAM_4 0x23050.
// Reading them answers whether this EE is enabled and whether any GSI firmware is there.
#define HGS_EE_STATUS(n) (0x23000u + 0x4000u * (n))
#define HGS_EE_HW_PARAM_2(n) (0x23040u + 0x4000u * (n))
#define HGS_EE_SW_VERSION(n) (0x23044u + 0x4000u * (n))
#define HGS_EE_HW_PARAM_4(n) (0x23050u + 0x4000u * (n))
#define HGS_EE_STATUS_ENABLED (1u << 0)
// Event channel block.
#define HGS_EV_CNTXT(n) (0x21000u + 0x4000u * (n))
#define HGS_EV_DB(n) (0x22100u + 0x4000u * (n))
#define HGS_EV_CMD(n) (0x23010u + 0x4000u * (n))
// Interrupt / status block.
#define HGS_TYPE_IRQ(n) (0x23080u + 0x4000u * (n))
#define HGS_TYPE_IRQ_MSK(n) (0x23088u + 0x4000u * (n))
#define HGS_SRC_CH_IRQ_CLR(n) (0x230A0u + 0x4000u * (n))
#define HGS_SRC_CH_IRQ(n) (0x23090u + 0x4000u * (n))
#define HGS_SRC_EV_CH_IRQ(n) (0x23094u + 0x4000u * (n))
#define HGS_SRC_EV_CH_IRQ_CLR(n) (0x230A4u + 0x4000u * (n))
#define HGS_SRC_IEOB_IRQ_CLR(n) (0x230C0u + 0x4000u * (n))
#define HGS_GLOB_IRQ_STTS(n) (0x23100u + 0x4000u * (n))
#define HGS_GLOB_IRQ_EN(n) (0x23108u + 0x4000u * (n))
#define HGS_GPII_IRQ_EN(n) (0x23120u + 0x4000u * (n))
#define HGS_INTSET(n) (0x23180u + 0x4000u * (n))
#define HGS_MSI_BASE_LSB(n) (0x23188u + 0x4000u * (n))
#define HGS_MSI_BASE_MSB(n) (0x2318Cu + 0x4000u * (n))
#define HGS_GPII_SCRATCH_0(n) (0x23400u + 0x4000u * (n))
#define HGS_GPII_SCRATCH_1(n) (0x23404u + 0x4000u * (n))
#define HGS_SRC_CH_IRQ_MSK(n) (0x23098u + 0x4000u * (n))
#define HGS_SRC_EV_CH_IRQ_MSK(n) (0x2309Cu + 0x4000u * (n))
#define HGS_SRC_IEOB_IRQ_MSK(n) (0x230B8u + 0x4000u * (n))
#define HGS_ERRL(n) (0x23200u + 0x4000u * (n))

// Context sub-offsets (gpi.c enum CNTXT_OFFS).
#define HGS_C_CONFIG 0x00u
#define HGS_C_R_LENGTH 0x04u
#define HGS_C_RING_BASE_LSB 0x08u
#define HGS_C_RING_BASE_MSB 0x0Cu
#define HGS_C_RING_RP_LSB 0x10u
#define HGS_C_RING_RP_MSB 0x14u
#define HGS_C_RING_INT_MOD 0x20u
#define HGS_C_RING_MSI_LSB 0x28u
#define HGS_C_RING_MSI_MSB 0x2Cu
#define HGS_C_RING_RP_UPDATE_LSB 0x30u
#define HGS_C_RING_RP_UPDATE_MSB 0x34u

// CNTXT_0 fields.
#define HGS_C0_EL_SIZE(sz) ((uint32_t)(sz) << 24)
#define HGS_C0_CHSTATE(v) (((v) >> 20) & 0xfu)
#define HGS_C0_CHSTATE_MASK 0x00f00000u
#define HGS_C0_DIR_OUT 0x8u          // GPI_CHTYPE_DIR_OUT (bit 3)
#define HGS_C0_PROTO_GPI 0x2u
#define HGS_EV_C0_INTTYPE_IRQ (1u << 16)
#define HGS_EV_C0_CHTYPE_GPI_EV 0x2u

// Channel states (gpi.c enum gpi_ch_state).
#define HGS_ST_NOT_ALLOCATED 0x0u
#define HGS_ST_ALLOCATED 0x1u
#define HGS_ST_STARTED 0x2u
#define HGS_ST_STOPPED 0x3u
#define HGS_ST_ERROR 0xFu

// Commands (gpi.c enum gpi_cmd).
#define HGS_CH_ALLOCATE 0u
#define HGS_CH_START 1u
#define HGS_CH_STOP 2u
#define HGS_CH_RESET 9u
#define HGS_CH_DE_ALLOC 10u
#define HGS_EV_RESET 9u
#define HGS_EV_DE_ALLOC 10u

// TRE types and flag bits (gpi.c).
#define HGS_TRE_TYPE_DMA 0x10u
#define HGS_TRE_TYPE_GO 0x20u
#define HGS_TRE_TYPE_CONFIG0 0x22u
#define HGS_TRE_CHAIN 0x1u
#define HGS_TRE_IEOB 0x8u
#define HGS_TRE_IEOT 0x9u
#define HGS_TRE_LINK 0xBu
#define HGS_TRE_TYPE_SHIFT 16u

#define HGS_SPI_CMD_TX 1u
#define HGS_SPI_CMD_RX 2u
#define HGS_SPI_CMD_DUPLEX 3u

// Event types (gpi.c enum GPI_EV_TYPE) and transfer codes.
#define HGS_EV_XFER_COMPLETE 0x22u
#define HGS_EV_IMMEDIATE_DATA 0x30u
#define HGS_EV_STALE 0xFFu
#define HGS_TCE_EOT 2u
#define HGS_TCE_UNEXP_ERR 16u

// Serial engine registers used by geni_se_select_gpi_mode().
#define HGS_SE_M_IRQ_EN 0x614u
#define HGS_SE_M_IRQ_CLEAR 0x618u
#define HGS_SE_S_IRQ_CLEAR 0x648u
#define HGS_SE_DMA_TX_IRQ_CLR 0xC44u
#define HGS_SE_DMA_RX_IRQ_CLR 0xD44u
#define HGS_SE_GENI_DMA_MODE_EN 0x258u
#define HGS_SE_GSI_EVENT_EN 0xE18u
#define HGS_SE_IRQ_EN 0xE1Cu
#define HGS_SE_M_CMD_DONE (1u << 0)
#define HGS_SE_M_TX_FIFO_WM (1u << 30)
#define HGS_SE_M_RX_FIFO_WM (1u << 26)
#define HGS_SE_M_RX_FIFO_LAST (1u << 27)

#define HGS_POLL_TRIES 1000u         // * 100 us = 100 ms per command (mainline waits 250 ms;
                                     // shortened because init may probe 8 ee/gpii combinations)
#define HGS_XFER_TRIES 400000u       // * 1 us = 400 ms per transfer

static void hgs_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static volatile uint32_t *hgs_at(struct hgs_dma *g, uint32_t gpii_off) {
    return (volatile uint32_t *)(g->regs + (gpii_off - g->ee));
}
static uint32_t hgs_rd(struct hgs_dma *g, uint32_t off) { return *hgs_at(g, off); }
static void hgs_wr(struct hgs_dma *g, uint32_t off, uint32_t v) {
    *hgs_at(g, off) = v;
    hgs_barrier();
}
static uint32_t hgs_se_rd(struct hgs_dma *g, uint32_t off) {
    return *(volatile uint32_t *)(g->se + off);
}
static void hgs_se_wr(struct hgs_dma *g, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g->se + off) = v;
    hgs_barrier();
}

// One microsecond of busy wait on the virtual counter. The engine needs tens of
// microseconds between a command and its state change, so the accuracy of cntfrq
// is irrelevant as long as the loop is monotonic.
static void hgs_wait_us(unsigned us) {
    uint64_t freq = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    uint64_t ticks = (freq ? freq : 19200000ull) * us / 1000000ull + 1ull;
    do { __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now)); } while (now - start < ticks);
}

static void hgs_sync(struct hgs_dma *g, const void *virt, size_t len, int op) {
    if (g->sync && virt && len) g->sync(g->sync_ctx, (void *)virt, len, op);
}

static int hgs_setrc(struct hgs_dma *g, int rc) {
    g->diag[HGS_D_RC] = (uint32_t)rc;
    return rc;
}

// Wait for a command to take effect. Mainline learns about it from the channel-control
// interrupt and then reads the state out of CNTXT_0, so accept either signal: the state
// field itself, or the corresponding SRC_CH_IRQ / SRC_EV_CH_IRQ bit (which the hardware
// sets whether or not the interrupt is masked).
static int hgs_wait_state(struct hgs_dma *g, uint32_t cntxt, uint32_t want,
                          uint32_t src_off, uint32_t src_bit, unsigned tries) {
    for (unsigned i = 0; i < tries; ++i) {
        uint32_t raw = hgs_rd(g, cntxt + HGS_C_CONFIG);
        uint32_t st = HGS_C0_CHSTATE(raw);
        if (st == want) return HGS_OK;
        if (st == HGS_ST_ERROR) return HGS_ERR_IO;
        if (src_off && src_bit && (hgs_rd(g, src_off) & src_bit)) return HGS_OK;
        hgs_wait_us(100);
    }
    return HGS_ERR_TIMEOUT;
}

static int hgs_chan_cmd(struct hgs_dma *g, unsigned ch, unsigned opcode) {
    hgs_wr(g, HGS_CH_CMD(g->gpii), ((uint32_t)opcode << 24) | (ch & 0xffu));
    return HGS_OK;
}

static void hgs_program_chan(struct hgs_dma *g, unsigned ch, unsigned dir_out,
                             void *ring, uint64_t phys, uint32_t len) {
    const unsigned n = g->gpii;
    const uint32_t base = HGS_CH_CNTXT(n, ch);
    (void)ring;
    // CHSTATE lives in bits 23:20 of CNTXT_0 and this engine reports the channel state
    // there (the event channel reads back 1 after its ALLOCATE). Mainline writes the whole
    // word - which puts 0 into CHSTATE - but it does so right after ALLOCATE and never
    // re-reads the field. Here the write is ordered before ALLOCATE and the field is
    // preserved on top, so either interpretation of the register is safe.
    uint32_t keep = hgs_rd(g, base + HGS_C_CONFIG) & HGS_C0_CHSTATE_MASK;
    // mainline gpi.c declares `u32 dir` in struct gchan and NEVER assigns it, so
    // GPII_n_CH_k_CNTXT_0(..., chan->dir, ...) always writes DIR = 0 - for the TX channel as
    // well as the RX one. This port had been setting GPI_CHTYPE_DIR_OUT on channel 0 out of
    // tidiness, i.e. it was the single field where our channel context differed from a
    // configuration known to work. Follow mainline exactly: EL_SIZE | PROTO, nothing else
    // (plus whatever CHSTATE the engine left there).
    (void)dir_out;
    hgs_wr(g, base + HGS_C_CONFIG, keep | HGS_C0_EL_SIZE(HGS_TRE_BYTES) | HGS_C0_PROTO_GPI);
    hgs_wr(g, base + HGS_C_R_LENGTH, len);
    hgs_wr(g, base + HGS_C_RING_BASE_LSB, (uint32_t)phys);
    hgs_wr(g, base + HGS_C_RING_BASE_MSB, (uint32_t)(phys >> 32));
    // Mainline writes the ring RP high word through the doorbell register (which
    // aliases CNTXT_5).
    hgs_wr(g, HGS_CH_DB(n, ch) + (HGS_C_RING_RP_MSB - HGS_C_RING_RP_LSB),
           (uint32_t)(phys >> 32));
    // SCRATCH_0 = pair<<16 | proto<<4 | seid. The pair is the other channel of the
    // instance; the GSI uses it to route a GO TRE with LINK set to the RX ring.
    hgs_wr(g, HGS_CH_SCRATCH(n, ch),
           ((uint32_t)(ch ? 0u : 1u) << 16) | (HGS_C0_PROTO_GPI << 4) | (g->seid & 0x7u));
    hgs_wr(g, HGS_CH_SCRATCH(n, ch) + 0x4u, 0);
    hgs_wr(g, HGS_CH_SCRATCH(n, ch) + 0x8u, 0);
    hgs_wr(g, HGS_CH_SCRATCH(n, ch) + 0xCu, 0);
    hgs_wr(g, HGS_CH_QOS(n, ch), 1);
    if (ch == 0) g->diag[HGS_D_SCRATCH] = hgs_rd(g, HGS_CH_SCRATCH(n, 0));
}

static void hgs_program_ev(struct hgs_dma *g) {
    const unsigned n = g->gpii;
    const uint32_t base = HGS_EV_CNTXT(n);
    const uint64_t phys = g->rings.ev_phys;
    hgs_wr(g, base + HGS_C_CONFIG,
           HGS_C0_EL_SIZE(HGS_TRE_BYTES) | HGS_EV_C0_INTTYPE_IRQ | HGS_EV_C0_CHTYPE_GPI_EV);
    hgs_wr(g, base + HGS_C_R_LENGTH, g->rings.ev_len);
    hgs_wr(g, base + HGS_C_RING_BASE_LSB, (uint32_t)phys);
    hgs_wr(g, base + HGS_C_RING_BASE_MSB, (uint32_t)(phys >> 32));
    hgs_wr(g, HGS_EV_DB(n) + (HGS_C_RING_RP_MSB - HGS_C_RING_RP_LSB), (uint32_t)(phys >> 32));
    hgs_wr(g, base + HGS_C_RING_INT_MOD, 0);
    hgs_wr(g, base + HGS_C_RING_MSI_LSB, 0);
    hgs_wr(g, base + HGS_C_RING_MSI_MSB, 0);
    hgs_wr(g, base + HGS_C_RING_RP_UPDATE_LSB, 0);
    hgs_wr(g, base + HGS_C_RING_RP_UPDATE_MSB, 0);
}

// geni_se_select_gpi_mode(): keep the engine's interrupt outputs silent and put it in
// DMA mode with the GSI event sources enabled.
static void hgs_se_gpi_mode(struct hgs_dma *g) {
    hgs_se_wr(g, HGS_SE_IRQ_EN, 0);
    hgs_se_wr(g, HGS_SE_M_IRQ_CLEAR, 0xffffffffu);
    hgs_se_wr(g, HGS_SE_S_IRQ_CLEAR, 0xffffffffu);
    hgs_se_wr(g, HGS_SE_DMA_TX_IRQ_CLR, 0xffffffffu);
    hgs_se_wr(g, HGS_SE_DMA_RX_IRQ_CLR, 0xffffffffu);
    uint32_t v = hgs_se_rd(g, HGS_SE_M_IRQ_EN);
    v &= ~(HGS_SE_M_CMD_DONE | HGS_SE_M_TX_FIFO_WM | HGS_SE_M_RX_FIFO_WM | HGS_SE_M_RX_FIFO_LAST);
    hgs_se_wr(g, HGS_SE_M_IRQ_EN, v);
    hgs_se_wr(g, HGS_SE_GENI_DMA_MODE_EN, hgs_se_rd(g, HGS_SE_GENI_DMA_MODE_EN) | 1u);
    hgs_se_wr(g, HGS_SE_GSI_EVENT_EN, hgs_se_rd(g, HGS_SE_GSI_EVENT_EN) | 0xfu);
    g->diag[HGS_D_SE_DMA] = hgs_se_rd(g, HGS_SE_GENI_DMA_MODE_EN);
    g->diag[HGS_D_SE_GSI] = hgs_se_rd(g, HGS_SE_GSI_EVENT_EN);
}

// Allocate + program + start both channels at ring index 0 (the initial bring-up).
//
// The v91 run showed this GPI never reflects ALLOCATE or START in CNTXT_0's CHSTATE
// field (it stays 0) while the SRC_* control-status bits do set, so completion is taken
// from those. A START that produces neither is *not* fatal here: the engine may already
// be running, and only a queued transfer can prove it - so the caller still proceeds and
// the event ring reports the truth. Each command's outcome is packed into HGS_D_TRACE.
static unsigned hgs_step_result(int rc) {
    if (!rc) return 0;
    return (rc == HGS_ERR_TIMEOUT) ? 1u : 2u;
}

// Force a channel back to "not allocated". Unused: pre == 0 proves the bootloader left
// nothing allocated, and a DE_ALLOC on a free channel only adds an error-log entry.
static void hgs_chan_clean(struct hgs_dma *g, unsigned ch) __attribute__((unused));
static void hgs_chan_clean(struct hgs_dma *g, unsigned ch) {
    const unsigned n = g->gpii;
    hgs_wr(g, HGS_SRC_CH_IRQ_CLR(n), 1u << ch);
    hgs_chan_cmd(g, ch, HGS_CH_DE_ALLOC);
    hgs_wait_us(2000);
    hgs_chan_cmd(g, ch, HGS_CH_RESET);
    hgs_wait_us(2000);
}

static int hgs_channels_start(struct hgs_dma *g) {
    const unsigned n = g->gpii;
    uint32_t trace = 0;
    // Was the channel already owned before we touched it? State 1 here means the
    // bootloader left it allocated and the clean-up below is what makes progress possible.
    g->diag[HGS_D_PRE] = hgs_rd(g, HGS_CH_CNTXT(n, 0));
    // No DE_ALLOC/RESET here any more: pre == 0 on every instance proves nothing was left
    // allocated by the bootloader, and deallocating a channel that was never allocated is
    // itself an error the engine logs (v95 showed ERROR_LOG already set before the first
    // real command, which then vetoed all four instances).
    // Mainline order is ALLOCATE first, context second - and v98 showed why: with the order
    // swapped, SCRATCH_0 read back 0 and CNTXT_0 lost its proto/dir bits, i.e. the ALLOCATE
    // *initialises* the channel context, so anything written before it is discarded.
    // What was probably wrong in v97 is the other half: writing CNTXT_0 as a whole word puts
    // 0 into CHSTATE (bits 23:20), and this engine does report the state there. So keep the
    // mainline order and make that write read-modify-write, then read the state back both
    // right after ALLOCATE (A0/A2) and after the context write (A1/A3).
    for (unsigned ch = 0; ch < 2; ++ch) {
        hgs_wr(g, HGS_SRC_CH_IRQ_CLR(n), 1u << ch);
        hgs_chan_cmd(g, ch, HGS_CH_ALLOCATE);
        int rc = hgs_wait_state(g, HGS_CH_CNTXT(n, ch), HGS_ST_ALLOCATED,
                                HGS_SRC_CH_IRQ(n), 1u << ch, HGS_POLL_TRIES);
        trace |= hgs_step_result(rc) << (2u * ch);
        if (ch == 0) {
            g->diag[HGS_D_A0] = hgs_rd(g, HGS_CH_CNTXT(n, 0));
            g->diag[HGS_D_E2] = hgs_rd(g, HGS_ERRL(n));
            g->diag[HGS_D_SRC] = hgs_rd(g, HGS_SRC_CH_IRQ(n));
        } else {
            g->diag[HGS_D_A2] = hgs_rd(g, HGS_CH_CNTXT(n, 1));
            g->diag[HGS_D_E3] = hgs_rd(g, HGS_ERRL(n));
        }
        if (rc) {
            g->diag[HGS_D_TRACE] = trace;
            return rc;
        }
        hgs_program_chan(g, ch, ch == 0,
                         ch == 0 ? g->rings.tx_virt : g->rings.rx_virt,
                         ch == 0 ? g->rings.tx_phys : g->rings.rx_phys,
                         ch == 0 ? g->rings.tx_len : g->rings.rx_len);
        if (ch == 0) g->diag[HGS_D_A1] = hgs_rd(g, HGS_CH_CNTXT(n, 0));
        else g->diag[HGS_D_A3] = hgs_rd(g, HGS_CH_CNTXT(n, 1));
    }
    // Did the ring length actually store? A zero length is a ring the engine cannot run.
    g->diag[HGS_D_RB] = hgs_rd(g, HGS_CH_CNTXT(n, 0) + HGS_C_R_LENGTH);
    g->diag[HGS_D_ERRL2] = hgs_rd(g, HGS_ERRL(n));
    for (unsigned ch = 0; ch < 2; ++ch) {
        hgs_wr(g, HGS_SRC_CH_IRQ_CLR(n), 1u << ch);
        hgs_chan_cmd(g, ch, HGS_CH_START);
        int rc = hgs_wait_state(g, HGS_CH_CNTXT(n, ch), HGS_ST_STARTED,
                                HGS_SRC_CH_IRQ(n), 1u << ch, HGS_POLL_TRIES);
        trace |= hgs_step_result(rc) << (4u + 2u * ch);
    }
    g->diag[HGS_D_TRACE] = trace;
    g->diag[HGS_D_E4] = hgs_rd(g, HGS_ERRL(n));
    // A START that never reports means this GPII instance cannot run our channels. Treat it
    // as a failed attempt so hgs_init walks on to the next instance instead of stopping at
    // GPII 1 - until now every build exercised exactly one instance and reported nothing.
    if (trace & 0xf0u) return HGS_ERR_TIMEOUT;
    // A command the engine rejects also raises CH_CTRL (which is why a set SRC bit alone
    // is not proof of success), so capture the error reporting here as well: GLOB bit0 is
    // the general-error interrupt and ERROR_LOG is what mainline dumps on failure.
    g->diag[HGS_D_IRQ_STTS] = hgs_rd(g, HGS_TYPE_IRQ(n));
    g->diag[HGS_D_GLOB] = hgs_rd(g, HGS_GLOB_IRQ_STTS(n));
    g->diag[HGS_D_ERRL] = hgs_rd(g, HGS_ERRL(n));
    g->tx_wp = 0;
    g->rx_wp = 0;
    // No doorbell here: gpi_start_chan() only sends START, and the first doorbell is
    // written by the first queued transfer (same as gpi_issue_pending).
    return HGS_OK;
}

// The transfer rings wrap implicitly: every transfer is drained before the next one is
// queued, and mainline lets the write pointer (and its doorbell) wrap below the ring
// base the same way. No ring-restart path is needed, which keeps the channel in the
// STARTED state the SE side was set up for.

static void hgs_put_tre(void *ring, unsigned idx, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    volatile uint32_t *t = (volatile uint32_t *)ring + (size_t)idx * 4u;
    t[0] = d0; t[1] = d1; t[2] = d2; t[3] = d3;
}

// The one mainline init function v92 skipped: gpi_config_interrupts(). Besides the masks
// it programs the EE's interrupt plumbing (INTSET, MSI base, GLOB/GPII enables, the
// GPII-level scratch registers). The masks below are the deliberate deviation - this board
// installs no handler for GPI lines 279..284 / 293..298, so TYPE_IRQ_MSK stays 0 and the
// engine never asserts. Everything else is written exactly as mainline does.
static void hgs_ee_setup(struct hgs_dma *g) {
    const unsigned n = g->gpii;
    hgs_wr(g, HGS_TYPE_IRQ_MSK(n), 0);
    hgs_wr(g, HGS_SRC_IEOB_IRQ_MSK(n), 0x1u);
    hgs_wr(g, HGS_SRC_CH_IRQ_MSK(n), 0x3u);
    hgs_wr(g, HGS_SRC_EV_CH_IRQ_MSK(n), 0x1u);
    hgs_wr(g, HGS_GLOB_IRQ_EN(n), 0xfu);
    hgs_wr(g, HGS_GPII_IRQ_EN(n), 0xfu);
    hgs_wr(g, HGS_MSI_BASE_LSB(n), 0);
    hgs_wr(g, HGS_MSI_BASE_MSB(n), 0);
    hgs_wr(g, HGS_GPII_SCRATCH_0(n), 0);
    hgs_wr(g, HGS_GPII_SCRATCH_1(n), 0);
    hgs_wr(g, HGS_INTSET(n), 1);        // level-IRQ set, not MSI
    hgs_wr(g, HGS_ERRL(n), 0);
}

static int hgs_try(struct hgs_dma *g) {
    const unsigned n = g->gpii;
    // Full mainline EE/interrupt setup (masks kept at zero, see hgs_ee_setup).
    hgs_ee_setup(g);
    // Did those writes actually land? If the SRC mask reads back 0 the engine will never
    // latch CH_CTRL for a command, which would explain "ALLOCATE works via CHSTATE but
    // START never reports anything".
    g->diag[HGS_D_MSK] = hgs_rd(g, HGS_TYPE_IRQ_MSK(n));
    g->diag[HGS_D_SRCM] = hgs_rd(g, HGS_SRC_CH_IRQ_MSK(n));
    g->diag[HGS_D_GLOBEN] = hgs_rd(g, HGS_GLOB_IRQ_EN(n));
    g->diag[HGS_D_GPIIEN] = hgs_rd(g, HGS_GPII_IRQ_EN(n));
    hgs_wr(g, HGS_SRC_CH_IRQ_CLR(n), 0x3u);
    hgs_wr(g, HGS_SRC_IEOB_IRQ_CLR(n), 0x1u);
    // Baseline for "did any command log an error": a non-zero read here means ERROR_LOG
    // cannot be cleared on this part, and only a *change* would be meaningful.
    g->diag[HGS_D_E0] = hgs_rd(g, HGS_ERRL(n));

    // v93's EV clean-up is gone for the same reason as the channel one.
    hgs_wr(g, HGS_SRC_EV_CH_IRQ_CLR(n), 1u);
    hgs_wr(g, HGS_EV_CMD(n), (HGS_CH_ALLOCATE << 24) | 0u);
    int rc = hgs_wait_state(g, HGS_EV_CNTXT(n), HGS_ST_ALLOCATED,
                            HGS_SRC_EV_CH_IRQ(n), 1u, HGS_POLL_TRIES);
    g->diag[HGS_D_TIRQ] = hgs_rd(g, HGS_TYPE_IRQ(n));
    g->diag[HGS_D_EVSRC] = hgs_rd(g, HGS_SRC_EV_CH_IRQ(n));
    g->diag[HGS_D_EV] = hgs_rd(g, HGS_EV_CNTXT(n));
    g->diag[HGS_D_E1] = hgs_rd(g, HGS_ERRL(n));
    // Do not give up here: whether the channels and the ring work is what matters, and an
    // EV-only complaint must not veto the instance (that is what cost v95 all four).
    (void)rc;
    hgs_program_ev(g);
    // Pre-seed the event ring pointers exactly like gpi_alloc_ev_chan(): wp starts one
    // slot behind base so the first recycled event wraps it onto base.
    g->ev_rp = 0;
    g->ev_wp = HGS_EV_ELEMS - 1u;
    hgs_wr(g, HGS_EV_DB(n), (uint32_t)(g->rings.ev_phys + (uint64_t)g->ev_wp * HGS_TRE_BYTES));

    rc = hgs_channels_start(g);
    if (rc) return rc;
    // Only a clean *allocate* phase proves the engine accepted the channel commands: a set
    // SRC bit can also mean "command rejected". The baseline is now ERRL2 (right after the
    // ALLOCATE pair) instead of E1, so an error that only a START logs no longer rejects an
    // otherwise usable instance - that is what made v98 report ok=0 for all four GPIIs and
    // hid the transfer result.
    if (hgs_rd(g, HGS_ERRL(n)) != 0 && g->diag[HGS_D_ERRL2] != 0) return HGS_ERR_IO;
    g->diag[HGS_D_CH0] = hgs_rd(g, HGS_CH_CNTXT(n, 0));
    g->diag[HGS_D_CH1] = hgs_rd(g, HGS_CH_CNTXT(n, 1));
    g->diag[HGS_D_EV] = hgs_rd(g, HGS_EV_CNTXT(n));
    return HGS_OK;
}

// ---------------------------------------------------------------------------
// apps_smmu: register the GPI's stream before the engine is allowed to touch memory.
//
// On this board the SMMU is left by the bootloader in the state UFS documented on
// 2026-10-17 (drivers/NativeUfs/NativeUfs.cpp): sCR0 = 0x002f0406, i.e. USFCFG = 1 -
// every stream that has no stream-match entry FAULTS. The GPI is behind stream 0xb6
// (`iommus = <&apps_smmu 0xb6 0x0>` in sm8750.dtsi) and nothing ever registered it, so
// the engine's own DMA faults the moment it touches a ring: ALLOCATE only writes
// registers and succeeds, START needs the ring and is therefore rejected with the channel
// pushed back to NOT_ALLOCATED - which is exactly the symptom chain v97..v101 recorded.
//
// The sequence is the one that already works for UFS (drivers/NativeUfs/SmmuTakeover.h,
// configureUfsSmmu), which is itself Linux's bypass_quirk: reserve a context bank, put
// SCTLR = 0 (stage 1 off) and CBAR.TYPE = S1_TRANS_S2_BYPASS in it, then map the stream to
// that bank with S2CR (written before SMR). Stage 2 bypass means physical addresses are
// used untranslated, which is why the rings can stay ordinary physical addresses.
#define HGS_SMMU_IDR0 0x020u
#define HGS_SMMU_IDR1 0x024u
#define HGS_SMMU_SMR(i) (0x800u + 4u * (i))
#define HGS_SMMU_S2CR(i) (0xc00u + 4u * (i))
#define HGS_SMMU_CBAR(b) (0x1000u + 4u * (b))
#define HGS_SMMU_CBA2R(b) (0x1800u + 4u * (b))
// Context bank registers live in GR1 at 0x8000, one page each - NOT right after CBAR/CBA2R.
// v105 crashed with a synchronous external abort (FAR = smmu + 0x2000, i.e. bank 1 under the
// old (pages + bank) * 4096 arithmetic) because 0x2000..0x7FFF is an unimplemented hole.
#define HGS_SMMU_GR1 0x8000u

static int hgs_smmu_bypass(struct hgs_dma *g, uint32_t sid) {
    if (!g->smmu) return -1;                       // no window mapped
    volatile uint32_t *m = (volatile uint32_t *)g->smmu;
    const uint32_t idr0 = m[HGS_SMMU_IDR0 / 4u];
    const uint32_t idr1 = m[HGS_SMMU_IDR1 / 4u];
    unsigned streams = idr0 & 0xffu;
    unsigned banks = idr1 & 0xffu;
    // Context-bank page size, Linux's rule (arm-smmu.c): IDR1 bit31 set means 64 KiB banks,
    // otherwise 4 KiB. UFS's planner used 1 << (((idr1 >> 28) & 7) + 1), which on this part
    // (idr1 = 0x60000053) yields 128 pages and makes the window check fail for no reason.
    const unsigned pages = (idr1 >> 31) & 1u ? 16u : 1u;
    g->diag[HGS_D_SMMU_IDR0] = idr0;
    g->diag[HGS_D_SMMU_IDR1] = idr1;
    if (!streams || banks < 2 || banks > 128) return -2;
    if ((pages + banks) * 4096u > 0xd0000u) return -2;
    if (streams > 128) streams = 128;

    // Pick a context bank that nobody has configured yet. Firmware may already own several,
    // and UFS takes one with this same bypass quirk, so a fixed index is a guess; scanning
    // for CBA2R = 0 and CBAR = 0 is the actual "free" condition (reset values).
    unsigned bank = 0xffffu;
    int reuse = 0;
    for (unsigned b = banks; b-- > 0u;) {
        if (!m[HGS_SMMU_CBA2R(b) / 4u] && !m[HGS_SMMU_CBAR(b) / 4u]) {bank = b; break;}
    }
    g->diag[HGS_D_SMMU_BANK] = bank;
    if (bank == 0xffffu) {
        // Every bank carries something. Several streams may share one bypass-quirk bank -
        // S2CR simply points at the same context bank - so reuse a bank that already is
        // "stage 1 off, stage 2 bypass" instead of failing.
        for (unsigned b = banks; b-- > 0u;) {
            const uint32_t cbOff = HGS_SMMU_GR1 + b * pages * 4096u;
            if (m[HGS_SMMU_CBAR(b) / 4u] == 0x10000u && m[cbOff / 4u] == 0) {
                bank = b; reuse = 1; break;
            }
        }
        g->diag[HGS_D_SMMU_BANK] = bank;
        if (bank == 0xffffu) return -3;
    }
    g->diag[HGS_D_SMMU_CBA2R] = m[HGS_SMMU_CBA2R(bank) / 4u];

    // First stream-match slot that is not valid yet (bit 31 clear).
    unsigned slot = streams;
    for (unsigned i = 0; i < streams; ++i)
        if (!(m[HGS_SMMU_SMR(i) / 4u] & 0x80000000u)) {slot = i; break;}
    if (slot == streams) return -4;

    const uint32_t cb = HGS_SMMU_GR1 + bank * pages * 4096u;
    if (!reuse) {
        m[cb / 4u] = 0;                            // SCTLR = 0: stage 1 disabled
        hgs_barrier();
        if (m[cb / 4u] != 0) return -5;
        m[HGS_SMMU_CBA2R(bank) / 4u] = 1u;         // VA64
        hgs_barrier();
        m[HGS_SMMU_CBAR(bank) / 4u] = 0x10000u;    // TYPE = S1_TRANS_S2_BYPASS
        hgs_barrier();
        if (m[HGS_SMMU_CBA2R(bank) / 4u] != 1u || m[HGS_SMMU_CBAR(bank) / 4u] != 0x10000u)
            return -6;
    } else if (m[HGS_SMMU_CBAR(bank) / 4u] != 0x10000u || m[cb / 4u] != 0) {
        return -6;
    }
    m[HGS_SMMU_S2CR(slot) / 4u] = bank;            // S2CR before SMR (arm_smmu_write_sme)
    hgs_barrier();
    if (m[HGS_SMMU_S2CR(slot) / 4u] != bank) return -7;
    m[HGS_SMMU_SMR(slot) / 4u] = 0x80000000u | (sid & 0xffffu);
    hgs_barrier();
    g->diag[HGS_D_SMMU] = slot | (bank << 16);
    g->diag[HGS_D_SMR] = m[HGS_SMMU_SMR(slot) / 4u];
    g->diag[HGS_D_S2CR] = m[HGS_SMMU_S2CR(slot) / 4u];
    g->diag[HGS_D_CBAR] = m[HGS_SMMU_CBAR(bank) / 4u];
    if (g->diag[HGS_D_SMR] != (0x80000000u | (sid & 0xffffu))) return -8;
    return HGS_OK;
}

int hgs_init(struct hgs_dma *g, uintptr_t regs, uintptr_t se, const struct hgs_rings *r,
             unsigned seid, hgs_sync_fn sync, void *sync_ctx) {
    if (!g) return HGS_ERR_PARAM;
    for (unsigned i = 0; i < HGS_D_MAX; ++i) g->diag[i] = 0;
    g->ready = 0;
    g->regs = regs;
    g->se = se;
    g->seid = seid;
    g->sync = sync;
    g->sync_ctx = sync_ctx;
    g->tx_wp = g->rx_wp = 0;
    g->ev_rp = g->ev_wp = 0;
    if (!regs || !se || !r) return hgs_setrc(g, HGS_ERR_PARAM);
    g->rings = *r;
    if (!r->ev_virt || !r->tx_virt || !r->rx_virt || !r->ev_phys || !r->tx_phys || !r->rx_phys)
        return hgs_setrc(g, HGS_ERR_PARAM);
    // Ring lengths must be powers of two that the engine can decode.
    if (r->ev_len < HGS_EV_BYTES || r->tx_len < HGS_TX_BYTES || r->rx_len < HGS_RX_BYTES)
        return hgs_setrc(g, HGS_ERR_PARAM);
    // Ring addresses must be aligned to their length (gpi_alloc_ring does the same).
    if ((r->ev_phys & (r->ev_len - 1u)) || (r->tx_phys & (r->tx_len - 1u)) ||
        (r->rx_phys & (r->rx_len - 1u)))
        return hgs_setrc(g, HGS_ERR_PARAM);

    // The engine's DMA has to be reachable before any command is issued: register the GPI
    // stream in apps_smmu first, otherwise START faults on its own ring (see hgs_smmu_bypass).
    g->diag[HGS_D_SMMU] = 0xffffffffu;
    g->diag[HGS_D_SMMU_RC] = (uint32_t)hgs_smmu_bypass(g, HGS_GPI_STREAM_ID);
    hgs_se_gpi_mode(g);

    // dma-channel-mask in the DT is 0x1e: instances 1..4 exist and the block stops
    // decoding above them (see HGS_EE_OFF). Linux picks the first free instance, which on
    // a bare-metal boot is 1; try the rest in case the bootloader left one claimed.
    int rc = HGS_ERR_TIMEOUT;
    g->ee = HGS_EE_OFF;
    // Read the EE's own identity once, before touching any channel: an ENABLED bit of 0 or a
    // zero SW_VERSION means no amount of command programming can work, and says why the
    // engine answers "unsupported inter-EE operation".
    g->diag[HGS_D_STAT] = hgs_rd(g, HGS_EE_STATUS(1));
    g->diag[HGS_D_SWVER] = hgs_rd(g, HGS_EE_SW_VERSION(1));
    g->diag[HGS_D_HWP2] = hgs_rd(g, HGS_EE_HW_PARAM_2(1));
    g->diag[HGS_D_HWP4] = hgs_rd(g, HGS_EE_HW_PARAM_4(1));
    uint32_t trAll = 0;
    for (unsigned n = 1; n <= 4; ++n) {
        g->gpii = n;
        g->tx_wp = g->rx_wp = 0;
        g->ev_rp = g->ev_wp = 0;
        rc = hgs_try(g);
        trAll |= (g->diag[HGS_D_TRACE] & 0xffu) << (8u * (n - 1u));
        if (!rc) {
            g->diag[HGS_D_TR_ALL] = trAll;
            g->diag[HGS_D_GPII] = n;
            g->diag[HGS_D_EE] = g->ee;
            g->ready = 1;
            g->diag[HGS_D_INIT_RC] = 0;
            return HGS_OK;
        }
    }
    g->diag[HGS_D_TR_ALL] = trAll;
    g->diag[HGS_D_GPII] = g->gpii;
    g->diag[HGS_D_EE] = g->ee;
    g->diag[HGS_D_CH0] = hgs_rd(g, HGS_CH_CNTXT(g->gpii, 0));
    g->diag[HGS_D_CH1] = hgs_rd(g, HGS_CH_CNTXT(g->gpii, 1));
    g->diag[HGS_D_EV] = hgs_rd(g, HGS_EV_CNTXT(g->gpii));
    g->diag[HGS_D_INIT_RC] = (uint32_t)rc;
    return hgs_setrc(g, rc);
}

// Parse one event ring element. Returns 1 when the transfer is complete, 0 to keep
// polling, negative on an engine error.
static int hgs_take_event(struct hgs_dma *g, unsigned idx, int *failed) {
    volatile uint32_t *e = (volatile uint32_t *)g->rings.ev_virt + (size_t)idx * 4u;
    const uint32_t d0 = e[0], d1 = e[1], d2 = e[2], d3 = e[3];
    const uint32_t type = (d3 >> 16) & 0xffu;
    const uint32_t chid = (d3 >> 24) & 0xffu;
    const uint32_t code = (d2 >> 24) & 0xffu;
    if (type != HGS_EV_XFER_COMPLETE) return 0;
    g->diag[HGS_D_EV0] = d0;
    g->diag[HGS_D_EV1] = d1;
    g->diag[HGS_D_EV2] = d2;
    g->diag[HGS_D_EV3] = d3;
    ++g->diag[HGS_D_EVN];
    if (code == HGS_TCE_UNEXP_ERR) {
        *failed = HGS_ERR_IO;
        return 1;
    }
    // Channel 0 is the TX ring and carries the GO TRE, so its completion is the end of
    // the SPI transaction (mainline completes on the TX descriptor as well).
    if (chid == 0) return 1;
    return 0;
}

int hgs_xfer(struct hgs_dma *g, const struct hgs_spi_cfg *cfg,
             const void *tx_virt, uint64_t tx_phys, void *rx_virt, uint64_t rx_phys, size_t len) {
    if (!g || !cfg) return HGS_ERR_PARAM;
    g->diag[HGS_D_RC] = 0;
    g->diag[HGS_D_EVN] = 0;
    g->diag[HGS_D_POLL] = 0;
    if (!g->ready) return hgs_setrc(g, HGS_ERR_PARAM);
    if (!len || len > 0xffffffu || !tx_phys || !rx_phys) return hgs_setrc(g, HGS_ERR_PARAM);

    // Both rings hold 64 elements and are fully drained between transfers, so a duplex
    // transfer (3 TREs on TX, 1 on RX) always fits; a descriptor may straddle the ring
    // end, which is exactly what mainline's wrapping write pointer does.

    const unsigned n = g->gpii;
    const unsigned tx0 = g->tx_wp;
    const unsigned tx1 = (tx0 + 1u) % HGS_CH_ELEMS;
    const unsigned tx2 = (tx0 + 2u) % HGS_CH_ELEMS;
    const unsigned rx_idx = g->rx_wp;

    // CONFIG0: word size, SPI mode, packing, clock source - the GPI programs the SE
    // from this TRE, which is why the SE word-length/packing registers are untouched.
    hgs_put_tre(g->rings.tx_virt, tx0,
                (cfg->word_len & 0x1fu) | ((uint32_t)cfg->cpha << 12) | ((uint32_t)cfg->cpol << 13) |
                    (1u << 24) | (1u << 25),                       // TX_PACK | RX_PACK
                0,
                (cfg->clk_div & 0xfffu) | ((cfg->clk_src & 0xfu) << 16),
                (HGS_TRE_TYPE_CONFIG0 << HGS_TRE_TYPE_SHIFT) | HGS_TRE_CHAIN);
    // GO: full duplex. CHAIN continues into the TX data TRE, LINK releases the paired
    // RX ring; rx_len stays 0 for duplex (mainline only sets it for SPI_RX).
    hgs_put_tre(g->rings.tx_virt, tx1,
                (((uint32_t)cfg->cs & 0x7u) << 8) | HGS_SPI_CMD_DUPLEX,
                0, 0,
                (HGS_TRE_TYPE_GO << HGS_TRE_TYPE_SHIFT) | HGS_TRE_CHAIN | (1u << HGS_TRE_LINK));
    // TX data.
    hgs_put_tre(g->rings.tx_virt, tx2,
                (uint32_t)tx_phys, (uint32_t)(tx_phys >> 32), (uint32_t)len,
                (HGS_TRE_TYPE_DMA << HGS_TRE_TYPE_SHIFT) | (1u << HGS_TRE_IEOT));
    // RX data: direction DEV_TO_MEM, so no IEOT (mainline sets it only for MEM_TO_DEV).
    hgs_put_tre(g->rings.rx_virt, rx_idx,
                (uint32_t)rx_phys, (uint32_t)(rx_phys >> 32), (uint32_t)len,
                (HGS_TRE_TYPE_DMA << HGS_TRE_TYPE_SHIFT));

    g->tx_wp = (tx0 + 3u) % HGS_CH_ELEMS;
    g->rx_wp = (rx_idx + 1u) % HGS_CH_ELEMS;
    g->diag[HGS_D_IDX] = (tx0 << 16) | rx_idx;

    hgs_sync(g, tx_virt, len, 0);                                            // data out
    hgs_sync(g, rx_virt, len, 1);                                            // drop stale lines
    hgs_sync(g, g->rings.tx_virt, g->rings.tx_len, 0);                       // TREs out
    hgs_sync(g, g->rings.rx_virt, g->rings.rx_len, 0);

    // Arm RX first, then kick TX: the GO TRE is what releases the paired RX ring.
    hgs_wr(g, HGS_CH_DB(n, 1), (uint32_t)(g->rings.rx_phys + (uint64_t)g->rx_wp * HGS_TRE_BYTES));
    hgs_wr(g, HGS_CH_DB(n, 0), (uint32_t)(g->rings.tx_phys + (uint64_t)g->tx_wp * HGS_TRE_BYTES));

    int done = 0, failed = 0;
    const uint32_t ev_base = g->rings.ev_phys;
    for (unsigned i = 0; i < HGS_XFER_TRIES && !done; ++i) {
        const uint32_t hw_rp = hgs_rd(g, HGS_EV_CNTXT(n) + HGS_C_RING_RP_LSB);
        g->diag[HGS_D_EV_RP] = hw_rp;
        unsigned hw_idx = g->ev_rp;
        if (hw_rp >= ev_base && hw_rp < ev_base + HGS_EV_BYTES)
            hw_idx = (hw_rp - ev_base) / HGS_TRE_BYTES;
        int progressed = 0;
        while (g->ev_rp != hw_idx) {
            hgs_sync(g, (uint8_t *)g->rings.ev_virt + (size_t)g->ev_rp * HGS_TRE_BYTES,
                     HGS_TRE_BYTES, 2);
            done = hgs_take_event(g, g->ev_rp, &failed);
            g->ev_rp = (g->ev_rp + 1u) % HGS_EV_ELEMS;
            g->ev_wp = (g->ev_wp + 1u) % HGS_EV_ELEMS;
            progressed = 1;
            if (done) break;
        }
        if (progressed)
            hgs_wr(g, HGS_EV_DB(n),
                   (uint32_t)(g->rings.ev_phys + (uint64_t)g->ev_wp * HGS_TRE_BYTES));
        g->diag[HGS_D_POLL] = i;
        if (!done) hgs_wait_us(1);
    }

    if (!done) {
        g->diag[HGS_D_IRQ_STTS] = hgs_rd(g, HGS_TYPE_IRQ(n));
        g->diag[HGS_D_GLOB] = hgs_rd(g, HGS_GLOB_IRQ_STTS(n));
        g->diag[HGS_D_ERRL] = hgs_rd(g, HGS_ERRL(n));
        g->diag[HGS_D_CH0] = hgs_rd(g, HGS_CH_CNTXT(n, 0));
        g->diag[HGS_D_CH1] = hgs_rd(g, HGS_CH_CNTXT(n, 1));
        g->diag[HGS_D_EV] = hgs_rd(g, HGS_EV_CNTXT(n));
        return hgs_setrc(g, HGS_ERR_TIMEOUT);
    }
    hgs_sync(g, rx_virt, len, 2);                                            // data in
    if (failed) return hgs_setrc(g, failed);
    return hgs_setrc(g, HGS_OK);
}

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
    hgs_wr(g, base + HGS_C_CONFIG,
           HGS_C0_EL_SIZE(HGS_TRE_BYTES) | (dir_out ? HGS_C0_DIR_OUT : 0u) | HGS_C0_PROTO_GPI);
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

static int hgs_channels_start(struct hgs_dma *g) {
    const unsigned n = g->gpii;
    uint32_t trace = 0;
    // Each command's completion flag must be dropped first: the SRC bit from ALLOCATE
    // would otherwise make the following START look complete the instant it is polled.
    for (unsigned ch = 0; ch < 2; ++ch) {
        hgs_wr(g, HGS_SRC_CH_IRQ_CLR(n), 1u << ch);
        hgs_chan_cmd(g, ch, HGS_CH_ALLOCATE);
        int rc = hgs_wait_state(g, HGS_CH_CNTXT(n, ch), HGS_ST_ALLOCATED,
                                HGS_SRC_CH_IRQ(n), 1u << ch, HGS_POLL_TRIES);
        trace |= hgs_step_result(rc) << (2u * ch);
        if (rc) {
            g->diag[HGS_D_TRACE] = trace;
            return rc;
        }
        hgs_program_chan(g, ch, ch == 0,
                         ch == 0 ? g->rings.tx_virt : g->rings.rx_virt,
                         ch == 0 ? g->rings.tx_phys : g->rings.rx_phys,
                         ch == 0 ? g->rings.tx_len : g->rings.rx_len);
        if (ch == 0) g->diag[HGS_D_SRC] = hgs_rd(g, HGS_SRC_CH_IRQ(n));
    }
    // Did the ring length actually store? A zero length is a ring the engine cannot run.
    g->diag[HGS_D_RB] = hgs_rd(g, HGS_CH_CNTXT(n, 0) + HGS_C_R_LENGTH);
    for (unsigned ch = 0; ch < 2; ++ch) {
        hgs_wr(g, HGS_SRC_CH_IRQ_CLR(n), 1u << ch);
        hgs_chan_cmd(g, ch, HGS_CH_START);
        int rc = hgs_wait_state(g, HGS_CH_CNTXT(n, ch), HGS_ST_STARTED,
                                HGS_SRC_CH_IRQ(n), 1u << ch, HGS_POLL_TRIES);
        trace |= hgs_step_result(rc) << (4u + 2u * ch);
    }
    g->diag[HGS_D_TRACE] = trace;
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

static int hgs_try(struct hgs_dma *g) {
    const unsigned n = g->gpii;
    // No GIC handler exists for this GPI, so mask every interrupt type before anything
    // can assert. A mask of zero means the engine only ever updates its status
    // registers, which is what the poll loops read.
    hgs_wr(g, HGS_TYPE_IRQ_MSK(n), 0);
    hgs_wr(g, HGS_SRC_CH_IRQ_CLR(n), 0x3u);
    hgs_wr(g, HGS_SRC_IEOB_IRQ_CLR(n), 0x1u);
    hgs_wr(g, HGS_ERRL(n), 0);          // mainline clears it with gpi_update_reg(.., U32_MAX, 0)

    hgs_wr(g, HGS_SRC_EV_CH_IRQ_CLR(n), 1u);
    hgs_wr(g, HGS_EV_CMD(n), (HGS_CH_ALLOCATE << 24) | 0u);
    int rc = hgs_wait_state(g, HGS_EV_CNTXT(n), HGS_ST_ALLOCATED,
                            HGS_SRC_EV_CH_IRQ(n), 1u, HGS_POLL_TRIES);
    g->diag[HGS_D_TIRQ] = hgs_rd(g, HGS_TYPE_IRQ(n));
    g->diag[HGS_D_EVSRC] = hgs_rd(g, HGS_SRC_EV_CH_IRQ(n));
    g->diag[HGS_D_EV] = hgs_rd(g, HGS_EV_CNTXT(n));
    if (rc) return rc;
    hgs_program_ev(g);
    // Pre-seed the event ring pointers exactly like gpi_alloc_ev_chan(): wp starts one
    // slot behind base so the first recycled event wraps it onto base.
    g->ev_rp = 0;
    g->ev_wp = HGS_EV_ELEMS - 1u;
    hgs_wr(g, HGS_EV_DB(n), (uint32_t)(g->rings.ev_phys + (uint64_t)g->ev_wp * HGS_TRE_BYTES));

    rc = hgs_channels_start(g);
    if (rc) return rc;
    g->diag[HGS_D_CH0] = hgs_rd(g, HGS_CH_CNTXT(n, 0));
    g->diag[HGS_D_CH1] = hgs_rd(g, HGS_CH_CNTXT(n, 1));
    g->diag[HGS_D_EV] = hgs_rd(g, HGS_EV_CNTXT(n));
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

    hgs_se_gpi_mode(g);

    // dma-channel-mask in the DT is 0x1e: instances 1..4 exist and the block stops
    // decoding above them (see HGS_EE_OFF). Linux picks the first free instance, which on
    // a bare-metal boot is 1; try the rest in case the bootloader left one claimed.
    int rc = HGS_ERR_TIMEOUT;
    g->ee = HGS_EE_OFF;
    for (unsigned n = 1; n <= 4; ++n) {
        g->gpii = n;
        g->tx_wp = g->rx_wp = 0;
        g->ev_rp = g->ev_wp = 0;
        rc = hgs_try(g);
        if (!rc) {
            g->diag[HGS_D_GPII] = n;
            g->diag[HGS_D_EE] = g->ee;
            g->ready = 1;
            g->diag[HGS_D_INIT_RC] = 0;
            return HGS_OK;
        }
    }
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

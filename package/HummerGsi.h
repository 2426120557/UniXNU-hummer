// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm GSI/GPI DMA engine for hummer's touch controller (spi4 on QUPv3 wrap 1).
//
// This serial engine advertises GENI_IF_DISABLE_RO (0x64) bit0 = 1, i.e. its PIO FIFO
// interface is disabled in hardware, and its DMA interface block (0x2004) is not
// CPU-accessible on this part. The only data path left - the one mainline
// spi-geni-qcom takes when the DT node carries `dmas = <&gpi_dma1 ...>` - is the GSI
// engine with GPI descriptors. Register map, ring/TRE layout and the init sequence
// follow drivers/dma/qcom/gpi.c and drivers/soc/qcom/qcom-geni-se.c (no code copied).
//
// Everything here is polled: this bring-up has no GIC handler for the GPI lines
// (279..284 / 293..298), so every interrupt source is masked and progress is read
// back from the context/event registers instead.
#ifndef HUMMER_GSI_H
#define HUMMER_GSI_H
#include <stdint.h>
#include <stddef.h>

#define HGS_OK 0
#define HGS_ERR_IO (-5)
#define HGS_ERR_PARAM (-22)
#define HGS_ERR_TIMEOUT (-110)

// Biggest transfer this engine is asked for is S3910_MAX_PAYLOAD + 3 (1027 bytes);
// the TRE length field is 24 bits, so nothing to worry about.
#define HGS_TRE_BYTES 16u
#define HGS_CH_ELEMS 64u                    // gpi.c CHAN_TRES
#define HGS_EV_ELEMS 256u                   // gpi.c CHAN_TRES << EV_FACTOR(2)

// Ring geometry as the probe must lay it out (all rings are pages of one physically
// contiguous, 4 KiB aligned buffer). Sizes are bytes and already powers of two, which
// the engine requires for the length register.
#define HGS_EV_BYTES (HGS_EV_ELEMS * HGS_TRE_BYTES)   // 4096
#define HGS_TX_BYTES (HGS_CH_ELEMS * HGS_TRE_BYTES)   // 1024
#define HGS_RX_BYTES (HGS_CH_ELEMS * HGS_TRE_BYTES)   // 1024
#define HGS_RING_BYTES (HGS_EV_BYTES + HGS_TX_BYTES + HGS_RX_BYTES)   // 6144

// Diagnostic slots, printed on the panic screen. One flash has to answer "how far did
// the engine get", so keep them ordered by the init/transfer sequence.
enum hgs_diag {
    HGS_D_GPII = 0,      // GPI instance that answered ALLOCATE
    HGS_D_EE,            // ee_offset that answered (0x10000 = sm6350-style, 0 = sdm845-style)
    HGS_D_INIT_RC,       // 0 = ready, else the failure code
    HGS_D_CH0,           // CH0 CNTXT_0 (state in bits 23:20) after init
    HGS_D_CH1,           // CH1 CNTXT_0
    HGS_D_EV,            // event channel CNTXT_0
    HGS_D_EV_RP,         // event ring read pointer the engine last published
    HGS_D_EV0,           // last completion event, dword 0 (TRE address)
    HGS_D_EV1,           // dword 1
    HGS_D_EV2,           // dword 2: length:24 | code<<24
    HGS_D_EV3,           // dword 3: status | type<<16 | chid<<24
    HGS_D_IRQ_STTS,      // CNTXT_TYPE_IRQ (must stay 0: all sources masked)
    HGS_D_GLOB,          // CNTXT_GLOB_IRQ_STTS (bit0 = error)
    HGS_D_ERRL,          // GPII error log
    HGS_D_SE_DMA,        // SE_GENI_DMA_MODE_EN (1 = engine in DMA mode)
    HGS_D_SE_GSI,        // SE_GSI_EVENT_EN
    HGS_D_SCRATCH,       // CH0 SCRATCH_0 (pair/proto/seid)
    HGS_D_IDX,           // tx ring write index << 16 | rx ring write index
    HGS_D_POLL,          // poll iterations the last transfer needed
    HGS_D_EVN,           // events seen during the last transfer
    HGS_D_RC,            // last hgs_xfer result
    HGS_D_MAX
};

// Cache maintenance hook: op 0 = clean (CPU wrote, engine reads), 1 = clean+invalidate,
// 2 = invalidate (engine wrote, CPU reads). The probe owns the policy.
typedef void (*hgs_sync_fn)(void *ctx, void *virt, size_t len, int op);

struct hgs_rings {
    void *ev_virt;  uint64_t ev_phys;  uint32_t ev_len;
    void *tx_virt;  uint64_t tx_phys;  uint32_t tx_len;
    void *rx_virt;  uint64_t rx_phys;  uint32_t rx_len;
};

struct hgs_spi_cfg {
    unsigned word_len;      // mainline peripheral.word_len = bits_per_word - 4
    unsigned cpol, cpha;
    unsigned clk_div;       // SER clock divider the SE was programmed with
    unsigned clk_src;       // DFS level (0 = TCXO on this board)
    unsigned cs;
};

struct hgs_dma {
    uintptr_t regs;         // GPI register window (device regs base, 0xa00000)
    uintptr_t se;           // serial engine window (0xa90000)
    unsigned ee;            // ee_offset in use (GPII offsets are relative to regs - ee)
    unsigned gpii;          // GPI instance in use (dma-channel-mask = 0x1e -> 1..4)
    unsigned seid;          // serial engine index (spi4 -> 4)
    int ready;
    unsigned tx_wp, rx_wp;  // transfer ring write indices (elements)
    unsigned ev_rp, ev_wp;  // event ring indices
    struct hgs_rings rings;
    hgs_sync_fn sync;
    void *sync_ctx;
    uint32_t diag[HGS_D_MAX];
};

int hgs_init(struct hgs_dma *g, uintptr_t regs, uintptr_t se, const struct hgs_rings *r,
             unsigned seid, hgs_sync_fn sync, void *sync_ctx);
// One full-duplex SPI transfer of len bytes through the GSI engine. tx/rx point at
// physically contiguous buffers the engine can reach by physical address.
int hgs_xfer(struct hgs_dma *g, const struct hgs_spi_cfg *cfg,
             const void *tx_virt, uint64_t tx_phys, void *rx_virt, uint64_t rx_phys, size_t len);
#endif

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
#define HGS_CH_ELEMS 64u                    // gpi.c CHAN_TRES, and the vendor DT's 64 in `dmas`
// The vendor node says qcom,ev-factor = <1> -> CHAN_TRES << 1 = 128 elements (2048 bytes),
// while mainline uses EV_FACTOR 2 (256 / 4096). Match the vendor: if the engine validates
// the event ring length against its own factor, a doubled ring is a rejected ALLOCATE.
#define HGS_EV_ELEMS 128u

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
    HGS_D_EE,            // ee_offset in use (0x10000 on this part, proven by the decode window)
    HGS_D_INIT_RC,       // 0 = ready, else the failure code
    HGS_D_PRE,           // CH0 CNTXT_0 before we touch anything (state in bits 23:20: 1 = already allocated)
    HGS_D_A0,            // CH0 CNTXT_0 immediately after its ALLOCATE (state 1 = the engine took it)
    HGS_D_A1,            // CH0 CNTXT_0 after the ring context was written (does the write keep state?)
    HGS_D_A2,            // CH1 CNTXT_0 immediately after its ALLOCATE
    HGS_D_A3,            // CH1 CNTXT_0 after the ring context was written
    HGS_D_RB,            // CH0 CNTXT_1 read back after programming (0x400 = the ring length stored)
    HGS_D_TRACE,         // per-command result, 2 bits each: alloc0 alloc1 start0 start1 (0 ok, 1 timeout, 2 error)
    HGS_D_TR_ALL,        // the same trace for every GPII tried, one byte per instance (1..4)
    HGS_D_TIRQ,          // CNTXT_TYPE_IRQ after the event-channel ALLOCATE (bit0 = CH_CTRL)
    HGS_D_SRC,           // CNTXT_SRC_CH_IRQ after the TX channel ALLOCATE (bit k = channel k changed)
    HGS_D_EVSRC,         // CNTXT_SRC_EV_CH_IRQ after the event-channel ALLOCATE
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
    HGS_D_E0,            // ERROR_LOG right after the initial clear (does clearing work at all?)
    HGS_D_E1,            // ERROR_LOG after the event-channel ALLOCATE
    HGS_D_E2,            // ERROR_LOG after channel 0 ALLOCATE
    HGS_D_E3,            // ERROR_LOG after channel 1 ALLOCATE
    HGS_D_E4,            // ERROR_LOG after both STARTs
    HGS_D_STAT,          // GSI_EE_n_GSI_STATUS (bit0 = ENABLED): is this EE enabled at all?
    HGS_D_SWVER,         // GSI_EE_n_GSI_SW_VERSION: 0 means no GSI firmware behind the block
    HGS_D_HWP2,          // GSI_EE_n_GSI_HW_PARAM_2
    HGS_D_HWP4,          // GSI_EE_n_GSI_HW_PARAM_4
    HGS_D_ERRL,          // GPII error log after the START attempts
    HGS_D_ERRL2,         // GPII error log right after the ALLOCATE pair
    HGS_D_MSK,           // CNTXT_TYPE_IRQ_MSK read back (0 here: the GIC line is deliberately kept off)
    HGS_D_SRCM,          // CNTXT_SRC_CH_IRQ_MSK read back (non-zero = CH_CTRL status can latch)
    HGS_D_GLOBEN,        // GLOB_IRQ_EN read back
    HGS_D_GPIIEN,        // GPII_IRQ_EN read back
    HGS_D_SMMU,          // apps_smmu stream result: slot | bank << 16 (0xffffffff = not attempted)
    HGS_D_SMR,           // stream-match register read back
    HGS_D_S2CR,          // stream-to-context register read back
    HGS_D_CBAR,          // context-bank attribute read back (0x10000 = S1_TRANS_S2_BYPASS)
    HGS_D_SMMU_RC,       // 0 = stream registered, else the step code that refused
    HGS_D_SMMU_IDR0,     // apps_smmu IDR0 (stream count in bits 7:0, EXIDS in bit 30)
    HGS_D_SMMU_IDR1,     // apps_smmu IDR1 (bank count in bits 7:0, page shift in bits 30:28)
    HGS_D_SMMU_BANK,     // context bank the scan settled on (0xffff = none free)
    HGS_D_SMMU_CBA2R,    // CBA2R of that bank before we touched it (0 = idle)
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

#define HGS_GPI_STREAM_ID 0xb6u     // sm8750.dtsi gpi_dma1: iommus = <&apps_smmu 0xb6 0x0>
#define HGS_QUP_STREAM_ID 0xa3u     // sm8750.dtsi qupv3_1: iommus = <&apps_smmu 0xa3 0x0>
#define HGS_SMMU_BANK_USED 0xfffffffeu  // HGS_D_SMMU sentinel: no free bypass bank

struct hgs_dma {
    uintptr_t regs;         // GPI register window (device regs base, 0xa00000)
    uintptr_t se;           // serial engine window (0xa90000)
    uintptr_t smmu;         // apps_smmu window (0x15000000); 0 = not mapped, skip stream setup
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

# GSI / GPI DMA port plan for hummer touch (spi4)

Status: **implemented** in `package/HummerGsi.{h,c}` (v90). This file keeps the hardware
facts and the reasoning; the code is in `HummerGsi.c` with the register map inline.

## What the driver does (v90)

- `hgs_init()`: selects the SE's GPI mode (`SE_IRQ_EN=0`, M_IRQ command/FIFO sources off,
  `SE_GENI_DMA_MODE_EN=1`, `SE_GSI_EVENT_EN |= 0xF`) and brings up the GPI: mask every
  interrupt type, allocate + program the event channel, allocate + program + start both
  transfer channels. Every step is polled on the context registers (no GIC).
- `hgs_xfer()`: builds CONFIG0 + GO(SPI_DUPLEX, CHAIN|LINK) + TX DMA on channel 0 and the
  RX DMA on channel 1, cleans the rings and buffers, doorbells RX then TX, then polls the
  event ring for the channel-0 completion event.
- `hgs_init()` tries ee_offset 0x10000 first (sm6350-compatible, what the sm8750 DT node's
  second compatible resolves to in the reference tree) and falls back to 0x0, and tries
  GPII 1..4. The two offsets address disjoint 64 KiB windows, so the probe is harmless.
- No SMMU programming: the rings are advertised by physical address, which is correct when
  the bootloader leaves apps_smmu stream 0xb6 bypassed. If a transfer never completes while
  every register reads back sane, that stream is the first thing to check.
- No clock/interconnect votes: `gpi_dma1` has no `clocks` property and UFS proves the NoC is
  pre-configured by the bootloader.

## Diagnostics on the panic screen (v90)

`HUMTOUCH gsi gpii.. ee.. irc.. rc.. ok.. ch0.. ch1.. ev.. evrp.. evn.. poll.. idx.. scr..`
`HUMTOUCH gsiev <completion event dwords> irq.. glob.. errl.. sdma.. sgsi..`

Reading: `irc` != 0 means init stopped at that code with `ch0/ch1/ev` showing the channel
states (CHSTATE in bits 23:20: 1 allocated, 2 started, 0xF error). `irc=0` but `rc=-110`
means the channels came up and the transfer alone failed - then `evn`/`ev`/`errl`/`glob`
say whether the GSI produced events, and `ch0/ch1` whether it stayed started.

## Why GSI is required (proven, do not re-litigate)

- `spi4: spi@a90000` in sm8750.dtsi has `dmas = <&gpi_dma1 0 4 QCOM_GPI_SPI>, <&gpi_dma1 1 4 QCOM_GPI_SPI>`
  and `dma-names = "tx","rx"` -> mainline `spi-geni-qcom` selects `GENI_GPI_DMA`.
- `GENI_IF_DISABLE_RO` (0x64) bit0 = 1 -> PIO FIFO is off in HW.
- v84..v87 proved: config all correct (SER clock CLK_EN=1 div2, DFS level0=TCXO, DMA_MODE_EN=0,
  M_IRQ_EN set), opcode 3 and 7 identical, engine latches M_ACTIVE but never moves a byte
  (irq/txfs/rxfs=0, rx stays 0xAA) -> -110.
- v89 proved: SE_DMA_IF_EN (0x2004) and SE_FIFO_IF_DISABLE (0x2008) are NOT CPU-accessible
  (read -> data abort, FAR = se.va + 0x2004) -> the SE-DMA path cannot be armed either.

So the only remaining path is GSI/GPI DMA.

## Hardware facts (sm8750)

- `gpi_dma1`: reg `0xa00000` size `0x60000`, compatible `qcom,sm6350-gpi-dma` (ee_offset = 0x10000).
  - Linux: `regs = 0xa00000`, `ee_base = regs - 0x10000`; all GPII offsets are relative to ee_base.
    So a register at GPII offset X lives at PA `0x9f0000 + X` (i.e. `0xa00000 + X - 0x10000`).
  - 12 channels, `dma-channel-mask = 0x1e`.
  - interrupts: GIC 279..284, 293..298 (level high).
  - **`iommus = <&apps_smmu 0xb6 0x0>`** -> GSI rings/descriptors are DMA-addressed through SMMU stream 0xb6.
- QUP wrapper `qupv3_1`: `0xac0000` size `0x2000`, `iommus = <&apps_smmu 0xa3>`.
- spi4 pins: gpio48/49/50 func `qup1_se4`, gpio51 CS. (unchanged from HummerTouchBoard.h)

## GPI register map (offsets from ee_base, n = gpii index, k = channel 0..1)

    CH_k_CNTXT_0     0x20000 + 0x4000*n + 0x80*k   // el_size<<24 | chstate<<20 | eridx<<14 | dir<<3 | proto
    CH_k_CNTXT_1     0x20004                        // ring length (bytes)
    CH_k_CNTXT_2/3   0x20008/0x2000C                // ring base phys lo/hi
    CH_k_CNTXT_4..7  0x20010..                        // rp/wp lo/hi
    CH_k_DOORBELL_0  0x22000 + 0x4000*n + 0x8*k     // write ring wp phys -> kicks channel
    CH_n_CMD         0x23008 + 0x4000*n             // opcode<<24 | chid
    CH_k_QOS         0x2005C + 0x4000*n + 0x80*k    // =1
    CH_k_SCRATCH_0   0x20060 + 0x4000*n + 0x80*k    // pair<<16 | proto<<4 | seid   (seid = SE index, spi4 -> 4)
    EV_CH_k_CNTXT_0  0x21000 + 0x4000*n + 0x80*k
    EV_CH_k_DOORBELL 0x22100 + 0x4000*n + 0x8*k
    EV_CH_CMD        0x23010 + 0x4000*n

  Channel cmd opcodes: ALLOCATE=0, START=1, STOP=2, RESET=9, DE_ALLOC=10.
  EV cmd opcodes: ALLOCATE=0, RESET=9, DE_ALLOC=10.
  CH_CNTXT_0: EL_SIZE bits31:24, CHSTATE 23:20, ERIDX 18:14, DIR bit3, PROTO 2:0 (GPI=0x2).
  DIR: ch0 = OUT(1) tx, ch1 = IN(0) rx.

## TRE (transfer ring element), 16 bytes = 4 dwords

Flags in dword3: TYPE=bits23:16, CHAIN=bit0, IEOB=bit8, IEOT=bit9, BEI=bit10, LINK=bit11.
TRE types: DMA=0x10, IMMEDIATE_DMA=0x11, GO=0x20, CONFIG0=0x22.

CONFIG0 TRE (only when spi->set_config, direction MEM_TO_DEV):
    d0 = word_len(4:0) | loopback(8) | cpol(13) | cpha(12) | tx_pack(24) | rx_pack(25)
    d2 = clk_div(11:0) | clk_src(19:16)
    d3 = TYPE=0x22 | CHAIN

GO TRE (direction MEM_TO_DEV):
    d0 = frag(26) | cs(10:8) | cmd(4:0)          // cmd: SPI_TX=1, SPI_RX=2, SPI_DUPLEX=3
    d2 = rx_len(23:0)
    d3 = TYPE=0x20 | (SPI_RX: IEOB|LINK ; SPI_TX: CHAIN ; SPI_DUPLEX: CHAIN|LINK)

DMA TRE:
    d0 = addr_lo, d1 = addr_hi, d2 = len(23:0)
    d3 = TYPE=0x10 | IEOT(direction==MEM_TO_DEV)  ; for <=8 byte writes may use IMMEDIATE_DMA=0x11

## Init / xfer sequence (from drivers/dma/qcom/gpi.c)

1. gpi_alloc_chan_resources: alloc ch transfer ring (phys-contiguous, power-of-2 len);
   gpi_ch_init:
   - alloc EV ring (CHAN_TRES << ev_factor elements, sizeof(gpi_event)=16)
   - config interrupts (TYPE_IRQ mask, SRC_CH/EV/IEOB masks, GLOB/GPII IRQ en, INTSET)
   - alloc EV chan (ALLOCATE cmd, program EV CNTXT, add events, ring doorbell)
   - for each of 2 channels: ALLOCATE cmd, program CH CNTXT (el_size/len/base/scratch/proto/seid/QOS)
   - for each channel: START cmd
2. prep_slave_sg: build TREs (CONFIG0 + GO + DMA), append to ch ring
3. issue_pending: write channel doorbell = new wp phys
4. IRQ (IEOB / EV): process event ring (xfer_complete / immediate_data), recycle events

## QUP side (SE) for GSI mode

- `GENI_GPI_DMA` mode: `SE_GENI_DMA_MODE_EN` bit0 = 0, `SE_GSI_EVENT_EN` = 1 (geni_se_select_mode).
- SE_GENI_GSI_* registers exist in newer geni-se (GSI IRQ/status) — need the SE-side GSI handshake.
- SE index for spi4 = 4 (seid in SCRATCH_0).

## Blocking risks (must resolve before committing days of work)

1. **SMMU stream 0xb6 (gpi_dma1) and 0xa3 (QUP wrapper).** UniXNU has no apps_smmu driver.
   Either (a) the bootloader already set up a bypass/identity mapping for these streams (then
   physical addresses work), or (b) we must program apps_smmu context + page tables. Unknown.
   -> FIRST STEP: probe whether GSI ring physical addresses can be used as-is.
2. **GSI firmware.** GSI runs firmware in its IRAM. If the ABL/UEFI does not load it, GSI is dead.
   -> FIRST STEP: read GSI global status / EE config to see if it is initialised.
3. **0x2000 SE block** (DMA_IF_EN / FIFO_IF_DISABLE) is CPU-inaccessible; GSI may need it set by fw.

## Suggested milestones

- M0 (probe): map 0xa00000 (and ee_base 0x9f0000), read GSI EE/status + QUP GSI regs, panic with
  values. Confirms GSI is alive and whether SMMU is pre-configured. One flash.
- M1: GSI channel ALLOCATE + EV ring + START for spi4 (no data), confirm CHSTATE goes STARTED.
- M2: minimal SPI_DUPLEX transfer of 4 bytes (T0x00 ff) with TX+RX rings, read event.
- M3: wire into tcmRead/tcmCommand (replace geni_spi_transfer FIFO).

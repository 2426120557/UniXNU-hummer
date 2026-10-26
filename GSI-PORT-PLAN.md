# GSI / GPI DMA port plan for hummer touch (spi4)

Status: **driver implemented (v90..v99), GSI proven alive, blocked on START.**
Read "Where it stands" and "Deviations from mainline" first; everything below is the
reasoning and the hardware facts established along the way.

## Where it stands after v97 (2026-10-26 ~03:00)

Proven on hardware:

| Fact | Evidence |
|---|---|
| The PIO FIFO cannot work | `GENI_IF_DISABLE_RO` bit0 = 1; v84..v87 showed config/clock/opcode all irrelevant (M_ACTIVE latches, nothing moves) |
| The SE-DMA block cannot be armed from the CPU | v89: reading `0x2004` raises a synchronous external abort, FAR = se.va + 0x2004 |
| The GPI register layout is right | writes store, contexts read back verbatim; decode window is exactly `0xa10000..0xa24000` = 5 x 0x4000 = GPII 0..4 |
| ee_offset = 0x10000 | vendor DT `qcom,gpi-ee-offset = <0x10000>`; window arithmetic matches only under the subtractive reading |
| GSI is alive | `gsiee st00000001 sw20070000` -> EE ENABLED, firmware 2.7 present, HW_PARAM readable |
| EV ALLOCATE + both channel ALLOCATEs are accepted | v97 `gsierr e0=e1=e2=e3=00000000` |
| START never completes | `tr00000050` (alloc0/1 = ok, start0/1 = timeout) and the event ring never advances |
| An error is logged *before* the first START | v97 `errl2` (read before the START pair) already equals `errl` = `0x1000a020` |

So the remaining question is narrow: **why does the START command not take effect, and what
logs the error between the RX context programming and the STARTs.**

## Deviations from mainline `gpi.c` (read this before changing anything)

The rule for this port: **reproduce mainline literally; deviate only where a measurement
proves the deviation is required, and write down which measurement.** Every deviation made
"because it looked reasonable" turned out to be a bug, so they are listed here explicitly.

Kept, each with a hard reason:

| Deviation | Why it is required |
|---|---|
| Polling instead of GPI interrupts | This board installs no handler for GIC 279..284 / 293..298 and the ADT publishes no such interrupt; every GPI interrupt source is masked so nothing asserts. Commands are therefore confirmed through `SRC_CH_IRQ` / `SRC_EV_CH_IRQ` and `CHSTATE`. |
| No SMMU programming | The vendor DT's `qup1_gpi_iommu_region` maps `0x0..0x100000` and `0x200000..0x1_00000000` identically, so physical ring addresses are correct as long as they avoid `0x100000..0x200000` (ours is at `0xcc344000`). |
| No clk / interconnect votes | `gpi_dma1` has no `clocks` property, and UFS runs with zero interconnect votes, so the bootloader has already configured the NoC. |
| Register writes instead of dmaengine | There is no Linux DMA/PM/devres framework in XNU; the sequences are reproduced by hand (`hgs_*`). |

Removed, all of them self-inflicted bugs (kept here as a warning):

| Removed deviation | What it cost |
|---|---|
| `DE_ALLOC` + `RESET` pre-clean before ALLOCATE | `pre == 0` had already proven the channels were free; deallocating a never-allocated channel makes the engine log `GSI_UNSUPPORTED_INTER_EE_OP_ERR`, which then vetoed every instance (v93/v95 wasted). |
| "`ERROR_LOG` grew" as the accept criterion | A set `SRC` bit means "command completed" *or* "command rejected", but the log can also grow for unrelated reasons; using it as a veto rejected all four GPIIs and reported `ok=0` (v98). Mainline judges by the channel-control interrupt plus `CHSTATE`; the port now uses `ERRL2` (the allocate phase only). |
| Writing the ring context **before** ALLOCATE | v98 proved ALLOCATE initialises the context: `SCRATCH_0` read 0 instead of `0x10024` and `CNTXT_0` lost the proto/dir bits. Mainline order (ALLOCATE, then context) is forced by hardware. |
| `ee_offset` probed with **writes** | Writing to the undeccoded `0xa27088` raised a synchronous external abort and panicked the kernel (v90). The right probe is read-only - `HW_PARAM_2` answers it safely, and the vendor DT states `0x10000` outright. |

Non-deviations worth remembering: mainline writes `CNTXT_0` as a whole word after ALLOCATE,
which puts 0 into `CHSTATE` (bits 23:20); mainline never re-reads that field, so it does not
notice. This port keeps the write read-modify-write so the state survives, and v99 reads it
back at two points (`A0/A2` right after ALLOCATE, `A1/A3` after the context write) to prove
which step breaks it.

Leads, in priority order:

1. **The vendor GPI driver.** Mainline `gpi.c` only ever touches offsets >= 0x20000, i.e. it
   assumes the bootloader already owns/configured the EE side. The vendor driver (with
   `qcom,static-gpii-mask`, `qcom,gpi-ee-offset`, `#dma-cells = <5>`) clearly does more.
   Getting that source would end the guessing; the OnePlusOSS repo only ships devicetree,
   so it has to come from a vendor kernel mirror.
2. **EE ownership.** Documented vendor behaviour: `qcom,gpii-mask = 0x1e` reserves GPII 0
   for firmware/hypervisor, and `qcom,gpi-ee-offset` separates EE 0 (Linux) from EE 1
   (hypervisor/TrustZone). A channel that is "allocated" but cannot be started looks a lot
   like an EE/ownership refusal - and the error log we read decodes to
   GSI_UNSUPPORTED_INTER_EE_OP_ERR in one of the two readings.
3. **Ring DMA reachability.** The vendor node wants the rings in `qup1_gpi_iommu_region`
   with `iommus = <&apps_smmu 0xb6>` and `dma-coherent`. That region's `iommu-addresses`
   is an identity map (`0x0..0x100000`, `0x200000..0x100000000`), so physical addresses are
   the right thing to advertise - but only if apps_smmu actually lets stream 0xb6 through.
   A blocked ring fetch would also produce "error logged, no events".
4. **Interrupt-based completion.** Everything is polled here because this board has no
   handler for GPI lines 279..284 / 293..298. Mainline completes commands through those
   interrupts; polling SRC bits works for ALLOCATE but not for START. This is the one
   deliberate deviation left, and it is a candidate for the START failure.

## What the driver does (v90..v97, package/HummerGsi.{h,c})

- `hgs_init()`: EE setup (masks zeroed, INTSET/GLOB/GPII enables, MSI base), event channel
  ALLOCATE + context, both channel ALLOCATE + context (`SCRATCH_0 = pair | proto | seid 4`),
  then START. Every step is polled and its ERROR_LOG snapshot kept (`gsierr e0..e4`).
- `hgs_xfer()`: CONFIG0 + GO(SPI_DUPLEX, CHAIN|LINK) + TX DMA on channel 0, RX DMA on
  channel 1, doorbell RX then TX, poll the event ring for the channel-0 completion event.
- `hgs_init` reads the EE's own identity first and prints it as `gsiee` - that read is what
  proved the engine is usable.

## Diagnostics on the panic screen

```
HUMTOUCH gsi   gpii.. ee.. irc.. rc.. ok.. pre.. rb.. tr.. tirq.. src.. evsrc..
HUMTOUCH gsich ch0.. ch1.. ev.. evrp.. evn.. poll.. idx.. scr..
HUMTOUCH gsiev <event dwords> irq.. glob.. errl.. errl2.. sdma.. sgsi..
HUMTOUCH gsierr e0.. e1.. e2.. e3.. e4..      (ERROR_LOG after clear / EV alloc / ch0 / ch1 / starts)
HUMTOUCH gsiee st.. sw.. hw2.. hw4..          (EE status: bit0 enabled, SW version, HW params)
```

`tr` packs 2 bits per command: alloc0, alloc1, start0, start1 (0 ok, 1 timeout, 2 error).

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

## v108 on the wire (2026-10-26)

First flash where the GPI accepted everything:

- `tr=00000000` — both ALLOCATEs and both STARTs reported done (every 2-bit step 0). The
  v108 fix (SCRATCH_0 = pair<<16 | **1**<<4 | seid, i.e. the SE protocol SPI rather than the
  CNTXT_0 chtype value 2) was the whole blocker: START is only accepted once the channel's
  scratch word names the protocol the engine has to drive.
- `scr=00010014` reads back, `gsierr e0..e4 = 0`, `smmu post=0`, `gsiee st=1 sw=20070000`.
- `gsiev ... evn=2` — two transfer-complete events per 4-byte read, so the TRE rings run.
- The transfer itself brings nothing back: `rx = 00 00 00 00` (marker miss, -71 TCM_EPROTO),
  `txfs/rxfs = 0`, and GPIO162 (active-low IRQ) reads 0 before and after the reset pulse.

The engine is now proven end to end, so the failure has moved off the GSI and onto the two
things that decide whether a byte can exist on the pads at all:

1. **`geni_se_init()` was never called.** mainline's `geni_se_init()` writes
   `SE_GENI_OUTPUT_CTRL = 0x7f` (the seven IO macros), `SE_GENI_FORCE_DEFAULT = 1`,
   `SE_GENI_CGC_CTRL |= 0x7f`, clears bit 0 of 0x644 and only then selects the DMA mode.
   Without those the GPI completes a transfer while the pads stay in their idle high-Z state
   — a completion event plus steady zeros on MISO is exactly what v108 shows. v109 adds the
   sequence (`hgs_se_gpi_mode`, copied from the proven reference driver) and prints `out/cgc`.
2. **The panel had no power at all.** `touchscreen@0` names
   `vddio-supply = <&vreg_l4b_1p8>` (PM8550B LDO4, 1.8 V) and
   `avdd-supply = <&vreg_l16b_2p8>` (LDO16, 3.104 V); both are RPMh regulators, so no TLMM
   register can switch them on. v109 adds `package/HummerRpmh.c` (apps RSC drv-2 @0x16520000,
   cmd-db @0x81c60000, one ACTIVE TCS, voltage/mode/enable) and votes `L4B_E0` 1800 mV plus
   `L16B_E0` 3104 mV before the reset pulse.

What v109's readout separates: `sesw out=0000007f` and `pwr ... rc0/0 dn=1` mean both fixes
landed; a real `S3910` IDENTIFY then means the panel is alive, while `irqa=1` with `rx` still
zero means power is up but the bus is not.

## v110: the panel answers (2026-10-26 19:00)

The rails were the whole story. With the cmd-db key discovered at runtime instead of
guessed, the readout is:

```
HUMTOUCH done s3/3 st0 e0
HUMTOUCH id rc0 via0 mode1 ver4428887 w1024 mx0 my0 obj0 cfg0 ...
HUMTOUCH x0 ffffffffff00/a5101c000000      <- 0xA5 read marker, real payload follows
HUMTOUCH x1 fffffffffffffff/5a50301015333  <- 0x5A continued-read + data
HUMTOUCH pwr ly0/0 n183 rc0/0 dn1           <- both votes completed (ly/a are stale diag
                                               fields, the vote itself was fine)
```

`tcmResetAndIdentify()` returned 0, i.e. the payload really contained "S3910" and the
controller is in application mode. Everything the bring-up needed before this - GSI
START, SE IO macros, 4-wire timing - had been right since v108/v109; what was missing
was 1.8 V + 3.1 V on the panel.

Why the guess failed while the runtime search worked: the bootloader's cmd-db keys its
VRM entries in one of several shapes (`L4B_E0` / `L4B_E1` / `L4B` / legacy `ldoB4`) and
only the device can say which. `hrpmh_scan()` now lists any `L4*`/`L16*` key it finds, so
the naming question can be answered from a screenshot instead of a guess.

Lessons for this bring-up:

- A GPI transfer that completes with a clean error log and returns all-zero data means
  "the far end is not powered" far more often than "the bus is wrong". The zero was
  literally a floating/unpowered MISO, and the active-low IRQ sitting at 0 was the same
  fact one pin over.
- Every board constant that differs from the reference device (SE base, pins, RSC base,
  cmd-db names) is a place where copying the reference driver cannot help. Those need to
  be *discovered*, not guessed - two flashes were spent guessing the one datum the
  hardware could have told us in a single read.

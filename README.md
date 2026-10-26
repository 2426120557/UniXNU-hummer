# UniXNU-hummer

OnePlus Ace 5 Pro（`hummer` / PKR110 / SM8750 骁龙 8 Elite）的 UniXNU 设备仓库。

> 整仓用 OnePlus 13T（Pagani）的移植骨架（同 SM8750、同 21H461 IPSW）：引擎 / 补丁 / 流水线全部用 13T 的
> （`upstream.rev = d9460ebb` + `patches/`），只保留 hummer 自己的设备相关部分：本机 UEFI 固件（hummerPkg）、
> GPT 几何、ADT、Synaptics S3910 触屏。

**2026-10-26：真机跑通 UEFI → XNU → 6 核 SMP → 真根卷 → 跳过 Setup 直进桌面 → 触屏可用。**

## 状态（v123，2026-10-26）

| 项 | 状态 | 验证版本 |
|---|---|---|
| UEFI 固件（hummerPkg） | ✅ 真机 `fastboot boot`（GOP 显示 + 按键输入） | v75 |
| 13T 全量流水线 | ✅ 编过，产 `payload.img`（160 MiB） | v75 |
| 根卷挂载 | ✅ `root_match_volume_uuid` 生效 | v75 |
| 用户态（Hello / SpringBoard） | ✅ `exit pid=1 init status=0` | v75 |
| 跳过 Setup | ✅ 直进桌面（不再停在 Hello） | v114 |
| 触屏（Synaptics S3910） | ✅ 可点可划，方向正确 | v112 / v113 |
| 6 核 SMP | ✅ `processor_count=6 avail=6` | v120 |
| 按键（音量/电源） | 🟡 引脚扫描版已提交，等真机读引脚号 | v123 |
| GPU（Adreno 830） | ⬜ 只有 `HummerAdrenoProbe.c` 探针 | — |
| WiFi | ⬜ | — |
| 8 核（2 个 Oryon prime） | ⬜ Aff2 拓扑未接 | — |
| 常驻刷入（替代 `fastboot boot`） | ⬜ | — |

## 五个坑（都已修，别再踩）

### 1. 6 核必须开 `sheng-smp=1`，而且从核要用内核向量表

v75 的 README 曾写「6 核不要用 `sheng-smp`，改用标准 PSCI」，**那是错的**：

- XNU 里没有任何 PSCI 代码（`osfmk/arm*` 搜不到 `psci`），起从核的是引擎的 IOCPU 驱动
  `drivers/ArmCpu/ArmCpu.cpp`；
- 那个 `probe()` 在**没有 `sheng-smp` 时对从核直接 `return nullptr`**，从核压根不注册。
  v75 所谓「6 核」只是 ADT 里声明了 6 个，从未被实测过——v115 第一次打
  `processor_count` 就是 **1**。

开 `sheng-smp=1` 之后会撞第二个坑：`start_cpu` 里给**每个核**都把 `VBAR_EL1` 指向引导器的
早期向量表（16 个入口全是 `b Lfatal`），而只有启动核后来会换回内核真向量表，**从核没有**。
于是从核一进 `machine_idle`（开着中断），第一个定时器中断就被当致命异常停住：

```
E01 0200/04 00000000 0b9f4c78
    │  │  │        └ ELR → __TEXT_EXEC 里的 _machine_idle
    │  │  └ ESR=0 → IRQ/FIQ 不写 ESR（不是"未定义指令"）
    │  └ SPSR=04 → EL1t
    └ MPIDR=0200 → cpu2，从核
```

修法在 `patches/unixnu-source-kernel.patch`：那段设 VBAR 的代码按 `x21`（cpu_data 指针，启动核为 0）
分流——启动核沿用引导器向量表并调 `early_boot_progress_mmu_on()`，从核改用内核
`ExceptionVectorsBase`（含 KVA 换算）。修完 v120 就是 `processor_count=6 avail=6`。

### 2. 触屏面板的两路电是 RPMh LDO，没有使能 GPIO

`sm8750-oneplus-hummer-bringup.dtsi` 的 `touchscreen@0`：

```dts
vddio-supply = <&vreg_l4b_1p8>;    // PM8550B LDO4,  1.8 V
avdd-supply  = <&vreg_l16b_2p8>;   // PM8550B LDO16, 3.104 V
```

**两路都是 RPMh 稳压器，SoC 里没有任何 GPIO 能开关它们。** 不投票 = 面板零电 = MISO 上永远读
`0x00`、GPIO162（低有效 IRQ）永远是 0。这条是 v108 之前所有失败的真正原因（GSI/SE 从 v108 起
就已经通了）。

`package/HummerRpmh.c` 复刻 Linux 的投票：apps RSC drv-2 @`0x16520000`、cmd-db @`0x81c60000`，
占一个空闲 ACTIVE TCS，写 电压/模式/使能 三条，每个写都回读。

### 3. cmd-db 里的电源资源名不能猜，要扫表

v109/v110 凭 `L4B_E0` 之类的名字去查，`rc-19`（`-ENOENT`）浪费了两版。实际表里的键必须运行时
扫。`hrpmh_scan()` 是只读遍历，把命中的键和 VRM 地址原样打出来。

实测结果：**cmd-db 头布局是 slave 基址 8 / 数据 144**（不是 16/144），
`L4B` @`0x0040800`、`L16B` @`0x0041900`，共 183 条。

### 4. GENI SE 要显式打开 IO 宏

`geni_se_init()` 那套：`SE_GENI_OUTPUT_CTRL = 0x7f`、`FORCE_DEFAULT = 1`、
`CGC_CTRL |= 0x7f`、`0x644` bit0 清零。没有它们，GPI 会"顺利完成"一次传输而焊盘仍处于高阻，
现象就是**完成事件正常 + 读回全 0**。屏上 `sesw out=0000007f` 就是验证点。

### 5. CI 缓存键必须含 `board.toml`

`key: hummer-13t-${{ hashFiles(...) }}` 里漏了 `board.toml` 时，只改启动参数**不会**让缓存失效，
`.cache/upstream-*` 里固化的旧 config 会被复用——payload 里的 `args` 还是上一版的（v121 空转
就是这么来的）。现在 `board.toml` 已在 key 里；另外 `doc` 类改动用 `[skip ci]` 提交，不触发编译。

## 另外两个操作教训

**早期异常屏只能画一行。** `drivers/BootProgress/` 的 MMU-off 早期路径在 hummer 这块屏上
`CELL=18px`，只能显示约 33 个字符，多行 dump 会全落到可见区外（现象：屏上只有标题一行）。
所以早期诊断信息必须压成**一行 ≤29 字符**（现在格式：`E<stage> <MPIDR>/<SPSR> <ESR> <ELR>`）。

**测 `processor_count` 不能在 `bsd_init` 里测。** 探针挂在 PMGR nub start 上，那时 IOCPU 的
从核还没匹配/启动，量到 1 是必然的。要起线程睡 15 秒再读（v117 的做法）。

## 目录

- `board.toml` — 板级描述。`[payload.adt]` 的 `cpu_regs` / `root_match_volume_uuid` /
  `packed_data_ramdisk` 是关键；`[payload].kernel_args` 是启动参数（编进 `UNIXNU.TOM`）。
- `configs/hummer_defconfig` — 内核配置。
- `scripts/make-hummer-adt.py` — Apple 设备树生成器（`cpu%d` + `/psci` + root-matching）。
- `scripts/build-hummer-touch.py` — 触屏归档（探针 + GeniSpi + GSI + RPMh）。
- `scripts/seed-data-skip-setup.py` — 在 Data 种子**副本**里写 "Setup 已完成" 偏好（原始种子不动）。
- `scripts/build-native-ufs.py` / `build-stage7-display.py` — 存储 / 软件显示栈。
- `drivers/BootProgress/` — 启动检查点 / panic 面板 / 早期异常输出（`BootProgress.c`、`Entry.S`）。
- `package/` — 板级内核侧代码：
  - `HummerTouchProbe.cpp` — 触屏 bring-up 分阶段探针 + 触点轮询 + HID 上报
  - `HummerGsi.c` — GPI/GSI DMA（TRE 环、SMMU bypass、SE 初始化）
  - `HummerRpmh.c` — apps RSC 投票（cmd-db 扫表 + ACTIVE TCS）
  - `HummerTouchBoard.h` — 全部板级常量（地址 / GPIO / 错误码）
- `patches/` — 13T 引擎补丁（内核 + NativeUfs + 显示）。
- `GSI-PORT-PLAN.md` — 触屏 bring-up 的完整过程记录（v100→v113 每一版屏上证据与判读）。
- `.github/workflows/build-kernel.yml` — CI（push `main` 自动编）。

## payload 结构（FAT32，160 MiB）

```
/EFI/UNIXNU/UNIXNU.TOM                 # 启动项：title / path / initrd / args
/EFI/UNIXNU/Payloads/kernel.macho      # 预链接 XNU 内核（~127 MB）
/EFI/UNIXNU/Payloads/kernel.macho.adt  # ADT：CPU 拓扑 + GICv3 + /psci + root-matching
/EFI/UNIXNU/Payloads/scratch-data.img  # 1 GiB Data ramdisk（跳过 Setup 的副本，压缩后 ~1 MB）
/EFI/UNIXNU/Payloads/*TRUST*           # trust cache
```

> boot 参数在 `UNIXNU.TOM` 的 `args` 字段里（**不是编译进内核**）。确认改动是否生效，最快的办法是
> `grep -a -o 'hummer-touch=[^ ]*' payload.img`。

## 打包 boot 镜像（WSL）

CI 只产 `payload.img`；boot 镜像在 WSL 里打（ABL **只收 v4 header**）：

```bash
bash dopack.sh v123 keyscan        # → ~/payload-v123.img + 桌面 boot-hummer-v123-keyscan.img
```

`dopack.sh` 调 `pack_v4.sh`：clang 编 BootShim（`FD_BASE=0xA7100000`、`FD_SIZE=0x300000`）→
`gzip(shim + SILICIUM_UEFI.FD) + DTB` 当 kernel → `payload.img` 当 ramdisk，`mkbootimg -header_version 4`。
成品自动复制到桌面，`fastboot boot` 验证。

## CI 与产物

- push `main` 自动触发 `build-kernel`（macOS runner）；纯文档改动请带 `[skip ci]`。
- 产物发布到 `ci-latest` rolling release：
  - `payload.img`（~160 MiB）
  - `kernel.macho`（带符号，可反查崩溃地址对应的函数）
  - `ci-logs.tar.gz`（全部步骤日志）
- 缓存：`out/ipsw`（~8 GiB，IPSW 只拉一次）、`.cache`（引擎树 + `xnu-work` 增量）、`out/feeds`。
  **缓存 key = `upstream.rev` + `patches/*` + `board.toml`**；改了其中任何一个都会重新 prepare。

## 关键硬件参数（真机实测）

| 项 | 值 |
|---|---|
| GICD / GICR | 0x16000000 / 0x16080000 |
| UFS hc / phy | 0x1d84000 / 0x1d80000 |
| DRAM base / size | 0x80000000 / 0x400000000 (16 GiB) |
| CPU | 6 性能核（Aff1 0x000..0x500）+ 2 Oryon prime（Aff2 0x10000/0x10100） |
| 触屏控制器 | Synaptics **S3910**，SPI = QUPv3 wrap1 @0x00a90000（SE4） |
| 触屏 GPIO | 复位 **161**、中断 **162**（低有效） |
| 触屏供电 | PM8550B **L4B** 1.8 V（vddio，VRM 0x40800）/ **L16B** 3.104 V（avdd，VRM 0x41900），无使能 GPIO |
| 触屏面板 | 1264 × 2780（TouchComm 报 `mx12639 my27799`，0.1 px 单位），10 触点对象，配置块 128 B |
| apps RSC / cmd-db | 0x16520000 / 0x81c60000，cmd-db 头 slave 8 / data 144 |
| GPU | Adreno 830 (a8xx)，KGSL 0x3d00000/0x40000，gpucc 0x3d90000/0x9800 |
| 调试串口 | qup_uart@0xa9c000（GENI SE9），115200n8，DTB `serial0` |

## 后续

1. **按键** — 先跑 `hummer-keyscan=1` 读出音量键焊盘编号（只读扫描输入位，按下保持 ~240 ms 就报），
   然后在 HID consumer collection 里补 `0xE9/0xEA/0x30`（音量±/电源）并发 report。电源键走 PMIC PON，
   需要 SPMI，放后面。
2. **GPU** — Adreno 830 / KGSL，SpringBoard 目前是纯 CPU 合成。
3. **WiFi** — 高通 WCN 系列，需要 PCIe + 固件下载。
4. **8 核** — 两个 Oryon prime 在 MPIDR **Affinity2**，而 XNU 拓扑只用 Aff1/0，且
   `MAX_CPU_CLUSTERS` 正好被 6 个核占满。要一起改：XNU 拓扑映射、`start.s` 的 MPIDR 掩码、
   ArmCpu 的 PSCI 亲和性掩码、ADT 生成器。

# UniXNU-hummer

OnePlus Ace 5 Pro（`hummer` / PKR110 / SM8750 骁龙 8 Elite）的 UniXNU 设备仓库。

> **v75 起整仓换成 OnePlus 13T（Pagani）的移植**（同 SM8750、同 21H461 IPSW）。引擎 / 补丁 / 流水线全部用 13T 的
> （`upstream.rev = d9460ebb` + `patches/`），只保留 hummer 自己的设备相关部分：本机 UEFI 固件（hummerPkg）、
> GPT 几何、ADT（6 核 + `/psci` + 根卷 UUID）、Synaptics S3910 触屏。

## 状态（v75，2026-10-25）

| 项 | 状态 |
|---|---|
| UEFI 固件（hummerPkg） | ✅ 真机 `fastboot boot` 成功（GOP 显示 + 按键输入） |
| 13T 全量流水线 | ✅ 编过：OS + SystemOS 提取 / ufs policies / ram-data / hummer-touch；产 `payload.img`（160 MiB） |
| 6 核 bring-up | ✅ 走标准 PSCI 起 6 核，早期异常消失 |
| 面板改名 | ✅ `OnePlus Ace 5 Pro`（不再是 `OnePlus 13T`） |
| root 挂载 | 🧪 已跑到 `root device wait`；补 `root_match_volume_uuid` 后验证中 |
| 持久 Data / GPU / 触摸控制器 | ⬜ |

## v75 踩到的三个坑（都已修，别再踩）

### 1. 6 核不要用 `sheng-smp`

`sheng-smp=1` 会在异常向量就绪（stage 6）**之前**拉起 secondary CPU / 触发中断，任何早期异常都会落进
`Entry.S` 的 `Lfatal`，面板显示 `OnePlus Ace 5 Pro / EARLY EXCEPTION`、`last checkpoint 0x1`、`ESR_EL1=0`。

改用标准 PSCI：ADT 带 `/psci`、`cpus=6`，**去掉** `sheng-smp` / `armcpu-spi-ipi`。从核由 XNU 在 `arm_init`
阶段经 PSCI 拉起，早期异常消失，内核一路跑到 root 挂载。

### 2. 根卷必须按 UUID 匹配

`board.toml` → `[payload.adt]` 必须有：

```toml
root_match_volume_uuid = "04769D26-2CA0-4825-B443-E2E5F88300F2"
```

这个 UUID 与内核 `patches/xnu/offline-owner-systemos-mount.patch` 里
`native_owner_publish_boot_manifest(root_device, "04769D26-…")` 用的完全一致。

缺了它 → ADT 的 `/chosen/root-matching` 为空 → XNU 回退到 `IOMedia Content=Apple_HFS` → 60 秒等不到 →
面板 `root value 0x3c`（0x3c = 60 秒超时）。

### 3. ADT 的 6 核本身是对的

真机 `hummer.dtb` 的 `/cpus` 就是 `cpu@0/100/200/300/400/500`（Aff1 `0x000..0x500`）+ `cpu@10000/10100`
（Aff2 两个 Oryon prime，按 sheng 惯例不列）。`/psci` = `arm,psci-1.0` / `smc`。
所以 6 核问题不在 MPIDR，而在**起核路径**。

## 目录

- `board.toml` — 板级描述。`[payload.adt]` 的 `cpu_regs` / `root_match_volume_uuid` / `packed_data_ramdisk` 是关键。
- `configs/hummer_defconfig` — 内核配置。
- `scripts/make-hummer-adt.py` — Apple 设备树生成器（6 核 `cpu%d` + `/psci` + root-matching）。
- `scripts/build-native-ufs.py` — NativeUfs 构建（root windows / 分区选择）。
- `scripts/build-stage7-display.py` — Stage 7 软件显示栈。
- `scripts/build-hummer-touch.py` — Synaptics S3910 触屏。
- `drivers/BootProgress/` — 启动检查点 / panic 面板 / 早期异常输出（`BootProgress.c`、`Entry.S`）。
- `patches/` — 13T 引擎补丁（内核 + NativeUfs + 显示）。
- `.github/workflows/build-kernel.yml` — CI（push `main` 自动编）。

## payload 结构（FAT32，160 MiB）

```
/EFI/UNIXNU/UNIXNU.TOM                 # 启动项：title / path / initrd / args
/EFI/UNIXNU/Payloads/kernel.macho      # 预链接 XNU 内核（~127 MB）
/EFI/UNIXNU/Payloads/kernel.macho.adt  # ADT：6 核 + GICv3 + /psci + root-matching
/EFI/UNIXNU/Payloads/scratch-data.img  # 1 GiB Data ramdisk（压缩后 ~1 MB）
/EFI/UNIXNU/Payloads/*TRUST*           # trust cache
```

> boot 参数在 `UNIXNU.TOM` 的 `args` 字段里（不是编译进内核），判断改动是否生效直接 grep payload 里的 `args` 最快。

## 打包 boot 镜像（WSL）

CI 只产 `payload.img`；boot 镜像在 WSL 里打（Ace 5 Pro 的 ABL **只收 v4 header**）：

```bash
bash pack_v4.sh <uefi_repo> <out.img> <payload.img>
```

`pack_v4.sh` 做三件事：clang 编 BootShim（`FD_BASE=0xA7100000`、`FD_SIZE=0x300000`）→
`gzip(shim + SILICIUM_UEFI.fd) + DTB` 当 kernel → `payload.img` 当 ramdisk，`mkbootimg --header_version 4`。
成品放桌面，`fastboot boot` 验证。

> WSL 的 `/tmp` 是 tmpfs，跨 `wsl.exe` 调用不保留 —— 输出直接写 `/mnt/c/Users/<你>/Desktop/`。

## CI 与产物

- push `main` 自动触发 `build-kernel`（macOS runner）。
- 产物发布到 `ci-latest` rolling release，公开可直接下：
  - `payload.img`（~160 MiB）
  - `kernel.macho`（带符号，可 `llvm-nm` 反查地址）
  - `ci-logs.tar.gz`（构建日志）
- 缓存：`out/ipsw`（~8 GiB，IPSW 只拉一次）、`.cache`（引擎树 + `xnu-work` 增量）、`out/feeds`。
  ⚠️ 引擎缓存 key 只含 `upstream.rev` + `patches/*`，**不含 `board.toml`** —— 并发 run 时可能读到旧 config
  （踩过一次：payload 里的 `args` 还是旧的）。改 `board.toml` 后最好确认 payload 里的 `args` 真的变了。

## 关键硬件参数（真机实测）

| 项 | 值 |
|---|---|
| GICD / GICR | 0x16000000 / 0x16080000 |
| UFS hc / phy | 0x1d84000 / 0x1d80000 |
| DRAM base / size | 0x80000000 / 0x400000000 (16 GiB) |
| CPU | 6 性能核（Aff1 0x000..0x500）+ 2 Oryon prime（Aff2 0x10000/0x10100） |
| GPU | Adreno 830 (a8xx)，KGSL 0x3d00000/0x40000，gpucc 0x3d90000/0x9800 |
| 调试串口 | qup_uart@0xa9c000（GENI SE9），115200n8，DTB `serial0` |

## 后续

root 挂载打通后进入 Stage 7 用户态；之后是持久 Data 卷、Adreno 830 GPU、Synaptics S3910 触摸控制器。

# UniXNU-hummer

OnePlus Ace 5 Pro（`hummer` / PKR110 / SM8750 骁龙 8 Elite）的 UniXNU 设备仓库。

## 状态

- ✅ **UEFI 层已完成**：固件含 UniXNU 菜单，真机 `fastboot boot` 成功（GOP 显示 + 按键输入均工作）。
- ✅ **双系统分区 + 根卷（2026-10-24）**：从 userdata 切出 32 GiB `unixnu` 分区，SHNGROOT 根卷镜像已刷入并经哈希校验；UFS bring-up → 窗口发布 → APFS 容器 → **真根卷挂载** 全部打通。
- ✅ **launchd 启动（v67，2026-10-24 真机验证）**：`vfs_mountroot mounted` → `load_init_program` → `exec init` → 签名/4K 全 rc=0 → `kboot exit rv=0`。XNU 已从真根卷把控制权交给用户态（bring-up 阶段 7）。用户态默认不往内核控制台输出，面板静默属预期。
- ✅ **面板检查点（kboot）+ GENI 调试串口（v68）**：kboot 系列探针覆盖 exec 尾段；`hummer-uart=1` 把内核日志多路复用输出到 DTB `serial0`（qup_uart@a9c000，115200n8）。
- 🧪 **Adreno 830 (a8xx)**：v68 起编入寄存器活性探针（`hummer-adreno-probe=1` 门控，复用引擎 bsd_init 钩子，零源码改动）；真显示驱动未开始。
- ⬜ 持久 Data 卷（launchd 挂载路径已有探针覆盖）、GPU 驱动 —— 进行中。

## 目录

- `board.toml` — 板级描述。`[firmware]` 已真机验证；`[kernel]/[ipsw]/[payload]` 为脚手架（TODO）。
- `configs/hummer_defconfig` — 内核配置（试水阶段最小集：XNU 4K 本体 + ArmDTPlatform + ArmCpu）。
- `scripts/make-hummer-adt.py` — Apple 格式设备树（ADT）生成器，本地可跑（纯 Python）。
- `scripts/build-arm-driver.py` — 从 UniXNU-sheng 拷贝的通用驱动构建器（ArmDTPlatform/ArmCpu 通用）。
- `.github/workflows/build-kernel.yml` — macOS CI 内核编译试水。

## 关键硬件参数（来自真机实测）

| 项 | 值 | 来源 |
|---|---|---|
| GICD / GICR | 0x16000000 / 0x16080000 | 真机 `/proc/iomem` |
| UFS hc / phy | 0x1d84000 / 0x1d80000 | 真机 `/proc/iomem` |
| DRAM base / size | 0x80000000 / 0x400000000 (16 GiB) | — |
| CPU | 6 性能核 + 2 Oryon 大核 | hummer.dts |
| GPU | Adreno 830 (a8xx)，KGSL 0x3d00000/0x40000，gpucc 0x3d90000/0x9800 | hummer.dts |
| 调试串口 | qup_uart@0xa9c000（GENI SE9），115200n8，DTB `serial0` | hummer.dts |

## CI 试水：怎么跑

1. **建 GitHub 仓库**（建议 **public** —— 公开仓库 macOS runner 免费不限时；私有仓库每月仅 200 macOS 分钟）：

   ```bash
   cd UniXNU-hummer
   git init
   git add .
   git commit -m "hummer device repo: UEFI verified + XNU kernel CI smoke"
   git branch -M main
   git remote add origin https://github.com/<你的用户名>/UniXNU-hummer.git
   git push -u origin main
   ```

2. **触发**：仓库 → Actions → `build-kernel` → Run workflow（或 push 自动触发）。

3. **预期**：第一次会花 ~20 分钟 `fetch`（浅克隆 xnu 等 Apple 源码）+ XNU 编译 30–90 分钟。
   - 内核 Mach-O 产出 → `✅ XNU kernel compiled`（prelink 因缺 IPSW kernelcache 失败是**预期内**）。
   - 失败 → 下载 `kernel-log` artifact，把关键报错发我，逐个补 hummer 的 `package/` kit。

## 本地构建 UEFI 固件（WSL，已跑通）

见 `apply_to_wsl.sh` + `fix_gic_feed.sh`（workspace 根目录）。核心命令：

```bash
cd ~/UniXNU
./unixnu defconfig hummer_defconfig -D <本仓库路径>
./unixnu build package firmware --uefi ~/uefi_oneplus_ace5_pro   # 跳过 macOS-only 的 kernel 步骤
bash ~/fix_gic_feed.sh                                            # 给 feed 版 mu_basecore 打 GIC spurious 补丁
bash ~/pack_v4.sh                                                 # v4 打包（Ace 5 Pro ABL 必须要 v4）
```

## 后续（另案）

SM8750 的 `package/` kit（平台专属驱动）、完整 I/O 驱动、持久数据卷、Adreno 830 GPU 驱动 —— 均在 CI 内核编译跑通之后。

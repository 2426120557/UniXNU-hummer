# hummer USB CDC 调试控制台（移植自 nabu）

目标：把 `Lux12138/nabu-usb-cdc`（Xiaomi Pad 5 / SM8150 上的 QcomXnuUsbDebug）搬到
OnePlus Ace 5 Pro（hummer / SM8750），让内核日志与 panic 走 USB CDC ACM，不再靠拍照。

## 已经核对过的 SM8750 硬件事实

全部取自本机参考树 `linux_oneplus_ace5_pro`（v7.2.9），不是猜的：

| 项 | 值 | 出处 |
|---|---|---|
| DWC3 | `0x0a600000`，窗口 `0xfc100` | `sm8750.dtsi` `usb@a600000` |
| GCC | `0x00100000`（USB 分支都在 `0x39xxx`） | `gcc-sm8750.c` |
| apps SMMU | `0x15000000`，`qcom,sm8750-smmu-500` | `sm8750.dtsi` `iommu@15000000` |
| USB stream id | **`0x40`**（`iommus = <&apps_smmu 0x40 0x0>`） | 同上 |
| HS PHY | **M31 eUSB2 `0x088e3000`**（`qcom,sm8750-m31-eusb2-phy`） | `sm8750.dtsi` `phy@88e3000` |
| USB3/DP PHY | `0x088e8000`（QMP combo，本移植不用） | `phy@88e8000` |
| GDSC | `gcc_usb30_prim_gdsc` @ `0x39004`，邻居 `0x39008`，**POLL_CFG_GDSCR** | `gcc-sm8750.c:2935` |
| BCR | `GCC_USB30_PRIM_BCR` @ `0x39000`；`GCC_QUSB2PHY_PRIM_BCR` @ `0x12000` | `gcc-sm8750.c:3146,3151` |
| 时钟分支（cbcr） | master `0x39018`、sleep `0x39028`、mock_utmi `0x3902c`、cfg_noc `0x3908c`、aggre `0x39090`、phy aux `0x39064`、com_aux `0x39068`、pipe `0x3906c` | `gcc-sm8750.c` |
| QSCRATCH | `dwc3_base + 0xf8800`，`HS_PHY_CTRL` 在 `+0x10` → **`0xf8810`** | `dwc3-qcom.c:715,26` |

### 与 nabu（SM8150）的差异

| 项 | nabu | hummer | 影响 |
|---|---|---|---|
| USB SID | `0x140` | `0x40`（与 engines 的 SM8550 profile 相同） | 只需换 profile 值 |
| HS PHY | femto `0x088e2000` | **M31 eUSB2 `0x088e3000`** | `QcomUsbPrepare.c` 里的 femto 序列**不能直接用**；本移植先不做 prepare |
| GDSC 状态位 | `GDSCR.PWR_ON` bit31（legacy） | `CFG_GDSCR.POWER_UP_COMPLETE` bit16 | `QUD_GDSC_LEGACY_PWR_ON 0` |
| clkref | GCC `0x8c008/0x8c028` | tcsrcc 的 `TCSR_USB2/USB3_CLKREF_EN` | GCC 窗口里没有，prepare 需要另开 TCSR 窗口，暂缓 |
| 启动交接 | ABL 留在 HOST 模式 + halted | 本机是 `fastboot boot` 起 XNU，**USB 应该已经在 DEVICE 模式跑着** | 先试"继承态"路径，可能连 prepare 都不用 |

## XNU 侧：补丁要 rebase，但底子很好

引擎 pin 的是 **xnu-10063.141.1**（`feeds.toml`），nabu 的 `patches/darwin22/` 是
**xnu-8796.141.3**。已在本机对真实源码做过 `git apply --check`：

- `0002-qcom-usb-debug-worker.patch` —— **干净**（`bsd_init.c` 两处偏移 -2 / -168）。
- `0001-qcom-usb-debug-console.patch` —— 两处失败：
  - `osfmk/kern/printf.c:908`
  - `osfmk/console/serial_console.c:515` —— 10063 的 `console_write()` 多了
    `disableConsoleOutput`、`console_suspended`、`pmap_in_ppl()` 三条早期返回分支，
    钩子的插入点要跟着挪。

所以 M2 的工作量就是重写这两个 hunk，不是重写补丁。

## 里程碑

| 阶段 | 内容 | 需要真机？ |
|---|---|---|
| **M0（本轮）** | 模块 vendor 进 `package/usbcdc/`、写 `soc/Sm8750.h`、`scripts/build-hummer-usbcdc.py` 用 osfmk 记录编译并归档；**只编不链** | 否，CI 就够 |
| M1 | boot arg 触发 `qcom_usb_debug_init()` + 把 `qcom_usb_debug_status` 打到 panic 屏；回答"ABL 有没有留 device 模式 / SID 0x40 有没有路由" | 是 |
| M2 | rebase 两个补丁 → `patches/xnu/`，`CONFIG_XNU_EXTRA_LD_ARCHIVES` force-load，`PE_kputc` 接通 → 主机出现 COM 口 | 是 |
| M3 | worker（CDC 轮询线程）、控制通道（`SXNU1` 帧：STATUS/REBOOT/SCREENSHOT）、截图 provider | 是 |

## 尚未验证、别写成事实的

- ABL 交给 XNU 时 DWC3 到底在什么模式（fastboot 说明 PHY 在跑，但 SMMU 路由与 core 状态未知）。
- 这块固件是否也拒绝 `S2CR.TYPE=BYPASS`（SM8150 读过 `0x10003 → 0x20003`）。profile 里
  `QUD_DMA_PREFER_IDENTITY=0`、`QUD_DMA_BYPASS_BANK=0`，等 M1 的 `dr/ds` 再改。
- `QUD_PHY_BASE` 故意没定义：`qcom-usb-prepare=1` 现在会返回 `QPREP_ARG`，别用。
- `QUD_PSCI_SMC_RESET=0`：hummer 的 SMC SYSTEM_RESET 没验证过，控制通道的重启会回 `nohook`。

## 模块来源

`package/usbcdc/` 是 `xiaoleGun/QcomXnuUsbDebug`（MIT，基线 `83be578`）经
`Lux12138/nabu-usb-cdc@627c83a` 的副本，除新增 `soc/Sm8750.h` 外未改动。详见
`package/usbcdc/README.md`。

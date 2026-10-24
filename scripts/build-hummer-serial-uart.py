#!/usr/bin/env python3
"""hummer: the GENI debug UART as a second serial console (TX only).

Why
---
v51 的面板控制台（build-hummer-console-probe.py）把渲染器注册进
pe_serial.c 的 gPESF 链表，kprintf 的多路复用把每个字符写给所有已注册
接口 —— 所以再注册一个真串口设备，内核日志就同时上面板 **和** 真 UART。
不需要动 PE_init_kprintf / pe_kprintf.c：serial_init() 的返回值本来就
因为面板而非零，PE_kputc 早已是 serial_putc_crlf。

UART 是谁
---------
DTB（从 boot-hummer 镜像里解出的 stock 设备树）:
    chosen: stdout-path = "serial0:115200n8"
    aliases: serial0 = /soc/qcom,qupv3_1_geni_se@ac0000/qcom,qup_uart@a9c000
    qcom,qup_uart@a9c000: compatible = "qcom,geni-debug-uart",
                           reg = <0xa9c000 0x4000>
    /soc ranges 恒等映射 => 0xa9c000 是绝对物理地址。
boot loader（ABL）已把这个 SE 留在 UART 调试模式，所以只需要轮询 TX。

寄存器与流程
------------
偏移量取 U-Boot 的 drivers/serial/serial_geni.c（公开参考实现），
GENI 寄存器图与 sheng 真机验证过的 GeniI2c.h 同族（同一 SM 引擎）：
    SE_GENI_STATUS 0x40 / M_CMD0 0x600 / M_IRQ_STATUS 0x610 /
    M_IRQ_CLEAR 0x618 / TX_WATERMARK 0x80c / TX_FIFO 0x700 /
    UART_TX_TRANS_LEN 0x270。
每个字符：置水印 -> 写 TRANS_LEN=1 -> M_CMD0=START_TX -> 轮询
M_TX_FIFO_WATERMARK_EN -> 写 TX_FIFO -> 清水印位 -> 轮询 M_CMD_DONE_EN
（超时则 ABORT）。轮询有界（100 万次），SE 不应答也不会挂死。

门控
----
boot arg `hummer-uart=1` 才注册（PE_parse_boot_argn，serial_init 里
已有同款用法）。默认不开：地址虽然来自本机 DTB，轮询写死的硬件行为
还是留一个开关比较稳。开了之后面板输出不变（多路复用），UART 115200 8N1。

幂等：两个锚点块各查一次自己的标记；pe_serial.c 不在任何引擎补丁的
文件清单里（b6ae99c 全部 51 个补丁核实），插入只受 console-probe 的
后置状态约束 —— 它先跑（board.toml 的 scripts 列表里排在前面）。
"""
import re
import subprocess
from pathlib import Path

MARKER = "hummer: geni debug uart"
XNU = Path(".cache/xnu-work")
PATCH_DIR = Path("patches/xnu")
PE_SERIAL = XNU / "pexpert/arm/pe_serial.c"
PROTOS = XNU / "pexpert/pexpert/arm/protos.h"

TOUCHED = (PE_SERIAL, PROTOS)

# ---------------------------------------------------------------------------
# 实现块：插在面板的 hummer_fbcon_serial_functions 之前（serial_init 之前），
# 与 console-probe 的产物共处一段但互不重叠。
IMPL_ANCHOR = """static struct pe_serial_functions hummer_fbcon_serial_functions =
{
\t.init = hummer_fbcon_init,
"""

IMPL_BLOCK = r'''/* hummer: GENI debug UART (DT: qcom,geni-debug-uart @ 0xa9c000, 0x4000;
 * chosen/stdout-path serial0:115200n8).  The boot loader left the engine in
 * UART mode, so only a poll-mode TX path is needed.  Offsets follow U-Boot's
 * serial_geni driver; the GENI register map is the same engine family sheng
 * verified in GeniI2c.h.  Bounded spins keep a silent engine from hanging
 * the console: a character that cannot be sent is dropped, not waited for. */
#define HU_UART_PHYS                 0xa9c000ULL
#define HU_UART_SIZE                 0x4000ULL
#define HU_SE_GENI_M_CMD0            0x600u
#define HU_SE_GENI_M_CMD_CTRL        0x604u
#define HU_SE_GENI_M_IRQ_STATUS      0x610u
#define HU_SE_GENI_M_IRQ_CLEAR       0x618u
#define HU_SE_GENI_TX_WATERMARK      0x80cu
#define HU_SE_UART_TX_TRANS_LEN      0x270u
#define HU_SE_GENI_TX_FIFO           0x700u
#define HU_M_TX_FIFO_WATERMARK_EN    (1u << 30)
#define HU_M_CMD_DONE_EN             (1u << 0)
#define HU_M_CMD_ABORT_EN            (1u << 5)
#define HU_M_GENI_CMD_ABORT          (1u << 1)
#define HU_UART_START_TX             0x1u
#define HU_M_OPCODE_SHIFT            27
#define HU_DEF_TX_WM                 2u
#define HU_UART_SPIN_LIMIT           1000000

static uint64_t hummer_uart_vaddr;
static bool      hummer_uart_registered;

static uint32_t
hummer_uart_rd(uint32_t reg)
{
	return *(volatile uint32_t *)(uintptr_t)(hummer_uart_vaddr + reg);
}

static void
hummer_uart_wr(uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)(uintptr_t)(hummer_uart_vaddr + reg) = val;
}

static bool
hummer_uart_poll(uint32_t reg, uint32_t bit)
{
	for (int i = 0; i < HU_UART_SPIN_LIMIT; i++) {
		if (hummer_uart_rd(reg) & bit) {
			return true;
		}
	}
	return false;
}

static void
hummer_uart_init(void)
{
	if (hummer_uart_vaddr == 0) {
		hummer_uart_vaddr = ml_io_map(HU_UART_PHYS, HU_UART_SIZE);
	}
}

static unsigned int
hummer_uart_transmit_ready(void)
{
	return hummer_uart_vaddr != 0;
}

static void
hummer_uart_transmit(uint8_t c)
{
	if (hummer_uart_vaddr == 0) {
		return;
	}

	hummer_uart_wr(HU_SE_GENI_TX_WATERMARK, HU_DEF_TX_WM);
	hummer_uart_wr(HU_SE_UART_TX_TRANS_LEN, 1);
	hummer_uart_wr(HU_SE_GENI_M_CMD0, HU_UART_START_TX << HU_M_OPCODE_SHIFT);

	if (!hummer_uart_poll(HU_SE_GENI_M_IRQ_STATUS, HU_M_TX_FIFO_WATERMARK_EN)) {
		return;
	}

	hummer_uart_wr(HU_SE_GENI_TX_FIFO, (uint32_t)c);
	hummer_uart_wr(HU_SE_GENI_M_IRQ_CLEAR, HU_M_TX_FIFO_WATERMARK_EN);

	if (!hummer_uart_poll(HU_SE_GENI_M_IRQ_STATUS, HU_M_CMD_DONE_EN)) {
		hummer_uart_wr(HU_SE_GENI_M_CMD_CTRL, HU_M_GENI_CMD_ABORT);
		hummer_uart_poll(HU_SE_GENI_M_IRQ_STATUS, HU_M_CMD_ABORT_EN);
	}

	hummer_uart_wr(HU_SE_GENI_M_IRQ_CLEAR,
	    HU_M_TX_FIFO_WATERMARK_EN | HU_M_CMD_DONE_EN | HU_M_CMD_ABORT_EN);
}

static unsigned int
hummer_uart_receive_ready(void)
{
	return 0;
}

static uint8_t
hummer_uart_receive(void)
{
	return 0;
}

static struct pe_serial_functions hummer_uart_serial_functions =
{
	.init = hummer_uart_init,
	.transmit_ready = hummer_uart_transmit_ready,
	.transmit_data = hummer_uart_transmit,
	.receive_ready = hummer_uart_receive_ready,
	.receive_data = hummer_uart_receive,
	.device = SERIAL_HUMMER_UART
};

'''

# 注册块：紧跟 console-probe 的面板注册之后（serial_init 顶部，Apple 探测
# 之前）。门控 boot arg hummer-uart=1。
REGISTER_ANCHOR = """\t/* hummer: register the panel console before any Apple UART probing. */
\tif (!hummer_fbcon_registered && hummer_early_console_ready()) {
\t\thummer_fbcon_registered = true;
\t\tregister_serial_functions(&hummer_fbcon_serial_functions);
\t}
"""

REGISTER_BLOCK = """
\t/* hummer: register the GENI debug UART (0xa9c000) when hummer-uart=1.
\t * The multiplexer then feeds every character to the panel as well. */
\tif (!hummer_uart_registered) {
\t\tuint32_t hummer_uart_enable = 0;
\t\tPE_parse_boot_argn("hummer-uart", &hummer_uart_enable,
\t\t                   sizeof(hummer_uart_enable));
\t\tif (hummer_uart_enable != 0) {
\t\t\thummer_uart_registered = true;
\t\t\tregister_serial_functions(&hummer_uart_serial_functions);
\t\t}
\t}
"""

# protos.h 的枚举追加（console-probe 刚加了 FBCON=0x20）。
ENUM_ANCHOR = "\tSERIAL_HUMMER_FBCON=0x20\n"
ENUM_BLOCK = "\tSERIAL_HUMMER_FBCON=0x20,\n\tSERIAL_HUMMER_UART=0x40\n"

KEY = "SERIAL_HUMMER_UART"
MARK = "hummer: GENI debug UART"


class Patcher:
    """Edit files in memory, commit only if every anchor matched."""

    def __init__(self):
        self.pending = {}

    def read(self, path):
        if path not in self.pending:
            if not path.is_file():
                raise SystemExit(f"hummer-uart: {path} is missing")
            with open(path, "r", encoding="utf-8", newline="") as handle:
                self.pending[path] = handle.read().replace("\r\n", "\n")
        return self.pending[path]

    def write(self, path, text):
        self.pending[path] = text

    def swap(self, path, old, new, tag):
        text = self.read(path)
        n = text.count(old)
        if n != 1:
            raise SystemExit(
                f"error: hummer-uart: {tag}: expected exactly one anchor in "
                f"{path}, found {n}\n--- anchor ---\n{old}")
        self.write(path, text.replace(old, new, 1))

    def insert_before(self, path, anchor, block, tag):
        self.swap(path, anchor, block + anchor, tag)

    def insert_after(self, path, anchor, block, tag):
        self.swap(path, anchor, anchor + block, tag)

    def commit(self):
        for path, text in self.pending.items():
            with open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(text)
            print("hummer-uart: patched", path)


def check_reversibility():
    """Which UniXNU patches still reverse-apply cleanly against the work tree?

    build-xnu-4k-baseline.sh re-runs `git apply --reverse --check` for every
    patch on every pass; an insertion inside a hunk's context makes that check
    fail and the build stops later with a patch error that says nothing about
    this script.  Returns None when the check is not possible here.
    """
    if not PATCH_DIR.is_dir() or not (XNU / ".git").exists():
        return None
    include = []
    for path in TOUCHED:
        include += ["--include=" + path.relative_to(XNU).as_posix()]
    report = {}
    for patch in sorted(PATCH_DIR.glob("*.patch")):
        proc = subprocess.run(
            ["git", "-C", str(XNU), "apply", "--reverse", "--check"]
            + include + [str(patch.resolve())],
            capture_output=True)
        report[patch.name] = proc.returncode == 0
    return report


def main():
    if not PE_SERIAL.is_file():
        raise SystemExit(
            "hummer-uart: pexpert/arm/pe_serial.c missing: no XNU work tree "
            "(run build-hummer-console-probe.py first)")

    text = PE_SERIAL.read_text(encoding="utf-8", errors="replace")
    if KEY in text:
        print("hummer-uart: sources already patched")
        report = check_reversibility()
        broken = [name for name, ok in (report or {}).items() if not ok]
        if broken:
            print("warning: hummer-uart: these patches no longer "
                  "reverse-apply: " + ", ".join(broken))
        return

    if REGISTER_ANCHOR not in text:
        raise SystemExit(
            "error: hummer-uart: the panel console registration is not in "
            "pe_serial.c -- build-hummer-console-probe.py must run first "
            "(board.toml [kernel] scripts order)")

    p = Patcher()
    p.insert_before(PE_SERIAL, IMPL_ANCHOR, IMPL_BLOCK, "geni uart implementation")
    p.insert_after(PE_SERIAL, REGISTER_ANCHOR, REGISTER_BLOCK, "geni uart registration")
    p.swap(PROTOS, ENUM_ANCHOR, ENUM_BLOCK, "serial_device enum")

    for path, body in p.pending.items():
        if MARK not in body and path == PE_SERIAL:
            raise SystemExit(f"hummer-uart: instrumentation did not land in {path}")
        if KEY not in body and path == PROTOS:
            raise SystemExit(f"hummer-uart: {KEY} missing from {path}")
    p.commit()

    report = check_reversibility()
    broken = [name for name, ok in (report or {}).items() if not ok]
    if broken:
        raise SystemExit(
            "error: hummer-uart: these patches no longer reverse-apply after "
            "the UART insertion: " + ", ".join(broken))
    if report is not None:
        print(f"hummer-uart: all {len(report)} patches still reverse-apply")
    print("hummer-uart: GENI debug UART registered behind hummer-uart=1")


if __name__ == "__main__":
    main()

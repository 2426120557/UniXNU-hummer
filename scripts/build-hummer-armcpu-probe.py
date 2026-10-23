#!/usr/bin/env python3
"""hummer: ArmCpu 适配补丁（Aff2 prime 核误匹配 + SMP 探针标记）。

依据（文档 / 参考实现 / 上游源码，2026-10-23）：

1. docs/zh/xnu/boot-arguments.md:
     sheng-smp=1      启动 secondary CPU
     armcpu-spi-ipi=1 使用 SPI 路径发送核间中断
2. UniXNU-sheng board.toml [payload] kernel_args 同时带有这两项；hummer 此前一项都没有，
   所以 ArmCpu::probe 把所有 secondary nub 拒掉（"Secondary startup is opt-in"）。
3. drivers/ArmCpu/ArmCpu.cpp (gitlink 1951cc8) probe():
     if (ml_get_cpu_number (id) != ml_get_boot_cpu_number ()) { ... opt-in ... }
4. osfmk/arm64/machine_routines.c ml_get_cpu_number():
     phys_id &= MPIDR_AFF1_MASK | MPIDR_AFF0_MASK;   // 只看 AFF1|AFF0
5. make-hummer-adt.py DEFAULT_CPU_REGS（v50 起照 sheng 参考收编）:
     "0,0x100,0x200,0x300,0x400,0x500"   —— 6 个 AFF1|AFF0 值域内的核
   SM8750 的 2 个 Oryon prime 核在 Affinity2（0x10000/0x10100），已照
   UniXNU-sheng 的做法留在 ADT 外（sheng board.toml:223："the other two
   little cores are left off"；其生成器拒绝 reg > 0xffff）。
   v49 实测：ADT 8 核 + probe 拒掉 2 个 prime 后，AppleS8000CPU panic
   "CPU configuration in device tree is invalid"（DT 8 核 vs 6 核不匹配），
   所以 v50 让 DT 与可启动核数一致，本脚本里的 REJECT 分支退居防御。

4+5 => ml_get_cpu_number(0x10000) = 0x10000 & 0xFFFF = 0 = boot 的逻辑号：
prime nub 在 probe() 里被误判为 boot CPU、跳过 opt-in 检查、进入 start() 走第二遍
boot 分支 —— v48 照片上"两次注册都走 boot-cpu branch 然后静默"就是这个。
sheng（SM8550）8 核全在 AFF0/AFF1，踩不到这个坑，所以参考实现没有此适配。

本脚本做两件事，都通过构建树相对路径直接补丁 drivers/ArmCpu/ArmCpu.cpp（子模块
工作树文件可写；build-arm-driver.py ArmCpu 每次无条件重编，无需清 .o）：

A. 窄修 Aff2 误匹配：reg 非零但 AFF1|AFF0 全零的 nub 直接拒绝（cpus=8 配额不受
   影响，先跑通 6 核；把 prime 纳入留给下一版）。
B. SMP 探针标记（hummer_early_u32x，画面即串口）：
     armP nub id / armP nub ml# / armP nub REJECT aff2   —— probe() 每个候选 nub
     arm0 intc done / arm0 initcpu done / arm0 regsvc done —— boot 分支关键步
     arm2 fs ok                                           —— finishSecondary 注册成功
     arm3 st pre ps / arm3 st ps returned                 —— processor_start 前后
     arm4 pre psci / arm4 psci rc                         —— PSCI CPU_ON 前后与返回码
     arm5 sec enter / arm5 step6 / arm5 step7             —— 次核侧推进

幂等：看到 "hummer: armcpu probe" 标记即跳过。锚点必须唯一，失败时 dump 上下文。

CI 实测教训（run 39872264785）：clang 拒绝块作用域的 `extern "C"` 声明
（"expected unqualified-id" 指向 "C"），所以声明只在文件顶部的全局声明区插一次
（上游自己在那里就有 extern "C" 的 processor_start 声明），块内只写调用。
"""
import re
from pathlib import Path

MARKER = "hummer: armcpu probe"
ARMCPU = Path("drivers/ArmCpu/ArmCpu.cpp")

# 全局声明锚：ArmCpu.cpp 顶部的 extern "C" 声明区（上游自己的 processor_start 就在此）。
DECL_ANCHOR = 'extern "C" kern_return_t processor_start (processor_t);\n'
DECL_NEW = (DECL_ANCHOR +
            '/* hummer: panel console renderer (arm_vm_init.c). */\n'
            'extern "C" void hummer_early_u32x (const char *, unsigned int);\n')


class Patcher:
    """Edit files in memory, commit only if every anchor matched."""

    def __init__(self):
        self.pending = {}

    def read(self, path):
        if path not in self.pending:
            if not path.is_file():
                raise SystemExit(f"armcpu-probe: {path} is missing "
                                 "(is the drivers submodule checked out?)")
            with open(path, "r", encoding="utf-8", newline="") as handle:
                self.pending[path] = handle.read().replace("\r\n", "\n")
        return self.pending[path]

    def write(self, path, text):
        self.pending[path] = text

    def swap(self, path, old, new, tag):
        text = self.read(path)
        n = text.count(old)
        if n != 1:
            self._diagnose(path, old, tag, n)
            raise SystemExit(
                f"error: armcpu-probe: {tag}: expected exactly one anchor in {path}, found {n}")
        self.write(path, text.replace(old, new, 1))

    @staticmethod
    def _diagnose(path, old, tag, n):
        print(f"error: armcpu-probe: {tag}: {n} match(es) in {path}", flush=True)
        words = re.findall(r"[A-Za-z_][A-Za-z0-9_]{6,}", old)
        if not words:
            return
        needle = words[0]
        text = open(path, "r", encoding="utf-8").read()
        idx = text.find(needle)
        if idx < 0:
            print(f"error: armcpu-probe: '{needle}' appears nowhere in {path}", flush=True)
            return
        print(f"error: armcpu-probe: context around first '{needle}':", flush=True)
        start = text.rfind("\n", 0, max(0, idx - 500))
        for line in text[start + 1:idx + 700].splitlines():
            print("error: armcpu-probe: | " + line, flush=True)

    def commit(self):
        for path, text in self.pending.items():
            with open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(text)


def main():
    if not ARMCPU.is_file():
        raise SystemExit(f"armcpu-probe: {ARMCPU} missing: no drivers checkout")
    if MARKER in Patcher.read(Patcher(), ARMCPU):
        print("armcpu-probe: ArmCpu.cpp already patched")
        return

    p = Patcher()

    # ---- 0. 全局 extern "C" 声明（块内声明被 clang 拒绝，见文件头教训） ------
    p.swap(ARMCPU, DECL_ANCHOR, DECL_NEW, "global decl")

    # ---- A. probe(): 打印每个候选 nub，并拒绝 Aff2 误匹配 --------------------
    p.swap(ARMCPU,
           "  memcpy (&id, reg->getBytesNoCopy (), sizeof (id));\n"
           "  if (ml_get_cpu_number (id) != ml_get_boot_cpu_number ()) {\n",
           "  memcpy (&id, reg->getBytesNoCopy (), sizeof (id));\n"
           "  { hummer_early_u32x (\"armP nub id\", id);\n"
           "    hummer_early_u32x (\"armP nub ml#\", static_cast<unsigned int> (ml_get_cpu_number (id))); }\n"
           "  /* " + MARKER + ": ml_get_cpu_number() masks MPIDR to AFF1|AFF0, so the two Oryon\n"
           "   * prime cores (AFF2 = 1, reg 0x10000/0x10100 on SM8750) collapse onto logical 0\n"
           "   * and would enter start() as a second boot CPU.  Reject any reg whose AFF1|AFF0\n"
           "   * bits are zero while the full value is not: the topology cannot address it yet. */\n"
           "  if (id != 0 && (id & 0xffffu) == 0) {\n"
           "    { hummer_early_u32x (\"armP nub REJECT aff2\", id); }\n"
           "    return nullptr;\n"
           "  }\n"
           "  if (ml_get_cpu_number (id) != ml_get_boot_cpu_number ()) {\n",
           "probe aff2 guard")

    # ---- B. boot 分支关键步 ---------------------------------------------------
    p.swap(ARMCPU,
           "  gController = controller;\n",
           "  gController = controller;\n"
           "  { hummer_early_u32x (\"arm0 intc done\", logicalId); }\n"
           "  { hummer_early_u32x (\"v50 cpus6\", logicalId); }\n",
           "boot intc done")

    p.swap(ARMCPU,
           "  initCPU (true);\n",
           "  initCPU (true);\n"
           "  { hummer_early_u32x (\"arm0 initcpu done\", logicalId); }\n",
           "boot initcpu done")

    p.swap(ARMCPU,
           "  registerService ();\n  __atomic_store_n (&gBootRegistered",
           "  registerService ();\n"
           "  { hummer_early_u32x (\"arm0 regsvc done\", logicalId); }\n"
           "  __atomic_store_n (&gBootRegistered",
           "boot regsvc done")

    # ---- C. finishSecondary(): 注册成功 ---------------------------------------
    p.swap(ARMCPU,
           "  if (status != KERN_SUCCESS) { armcpu_smp_fail[logicalId] = 4; return false; }\n"
           "  setCPUNumber (info.log_id);",
           "  if (status != KERN_SUCCESS) { armcpu_smp_fail[logicalId] = 4; return false; }\n"
           "  { hummer_early_u32x (\"arm2 fs ok\", logicalId); }\n"
           "  setCPUNumber (info.log_id);",
           "finishSecondary reg ok")

    # ---- D. startThread(): processor_start 前后 -------------------------------
    p.swap(ARMCPU,
           "  armcpu_smp_step[self->logicalId] = 3;\n"
           "  // Blocks until the new core reaches the scheduler; a core that never arrives only stalls this helper thread.\n"
           "  processor_start (self->machProcessor);\n",
           "  armcpu_smp_step[self->logicalId] = 3;\n"
           "  { hummer_early_u32x (\"arm3 st pre ps\", self->logicalId); }\n"
           "  // Blocks until the new core reaches the scheduler; a core that never arrives only stalls this helper thread.\n"
           "  processor_start (self->machProcessor);\n"
           "  { hummer_early_u32x (\"arm3 st ps returned\", self->logicalId); }\n",
           "startThread bracket")

    # ---- E. startCPU(): PSCI CPU_ON 前后与返回码 -------------------------------
    p.swap(ARMCPU,
           "  armcpu_smp_step[logicalId] = 4;\n"
           "  int64_t result = psciCall (hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, 0);\n"
           "  armcpu_smp_psci_rc[logicalId] = static_cast<int32_t> (result);",
           "  armcpu_smp_step[logicalId] = 4;\n"
           "  { hummer_early_u32x (\"arm4 pre psci\", logicalId); }\n"
           "  int64_t result = psciCall (hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, 0);\n"
           "  { hummer_early_u32x (\"arm4 psci rc\", static_cast<unsigned int> (result)); }\n"
           "  armcpu_smp_psci_rc[logicalId] = static_cast<int32_t> (result);",
           "startCPU psci bracket")

    # ---- F. initCPU(secondary): 次核侧推进 -------------------------------------
    for step, tag in ((5, "arm5 sec enter"), (6, "arm5 step6"), (7, "arm5 step7")):
        p.swap(ARMCPU,
               f"    armcpu_smp_step[logicalId] = {step};\n",
               f"    armcpu_smp_step[logicalId] = {step};\n"
               f"    {{ hummer_early_u32x (\"{tag}\", logicalId); }}\n",
               f"initCPU step {step}")

    p.commit()
    print("armcpu-probe: patched " + str(ARMCPU))
    print("armcpu-probe: Aff2 masquerade guard + SMP marks installed")


if __name__ == "__main__":
    main()

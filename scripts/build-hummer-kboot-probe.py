#!/usr/bin/env python3
"""hummer: put the kernel's own startup checkpoints on the panel ("kboot").

依据（文档 / 上游补丁 / 已验证的探针机制，2026-10-23）：

1. docs/zh/guide/debugging + docs/zh/porting/bring-up：SMP 之后要看内核走到哪，
   官方手段是 boot-progress 诊断页（boot-progress=1/2）。
2. patches/xnu/zzz-boot-progress.patch（自动全量应用）：在内核启动路径插入了
   boot_progress_* 调用 —— kernel_bootstrap_log/kernel_bootstrap_thread_log
   （startup.c，覆盖全部内核 bootstrap 检查点）、bsd_init.c（bsd_init enter、
   vfs_mountroot enter/mounted、root authentication）、kern_exec.c（exec
   load_machfile、load_init_program enter）。osfmk/kern/boot_progress.h 里
   XNU_BOOT_PROGRESS=0 时这些调用是空宏，所以 hummer 的面板上看不到它们。
3. 打开 CONFIG_XNU_BOOT_PROGRESS=y 需要 baseline 的三个前置（OWNER_ROOT_AUDIT、
   NATIVE_VIRTIO_BLOCK 等）且 scripts/build-boot-progress.py 在上游公开仓库不存在
   （kits/sheng/BootProgress.c 同样未公开）—— 一次引入太多新变量。
4. 本脚本改为：保持 defconfig 不动，把每个 boot_progress_stage/process_stage
   检查点旁边插一行 hummer_early_prints("kboot", ...)。渲染器
   （hummer_early_prints，osfmk/arm64/arm_vm_init.c 提供，C 链接）由
   build-hummer-console-probe.py 装好；C 文件里块内 extern 原型合法
   （clang 拒绝的是 C++ 的块内 extern "C"，见 armcpu 探针的教训）。

锚点全部来自 zzz-boot-progress.patch 落盘后的文本（CI 树上补丁已应用）：
  - startup.c 的两个 log 函数体第一行是 boot_progress_stage(message);
  - 字面量调用 \tboot_progress_stage("...")（bsd_init enter / vfs_mountroot
    enter / mounted / load_init_program enter 等）与
    \tboot_progress_process_stage(proc_getpid(p),p->p_name,"...")
    （exec load_machfile）用正则逐行配对。

幂等：文件头有 "hummer: kboot probe" 注释即跳过。改动会让 startup.o /
bsd_init.o / kern_exec.o 过期，STALE_OBJECTS 把它们连同 .o.json 一起删掉强制重编。
"""
import re
from pathlib import Path

MARKER = "hummer: kboot probe"
XNU = Path(".cache/xnu-work")

STALE_OBJECTS = ("startup.o", "startup.o.json",
                 "bsd_init.o", "bsd_init.o.json",
                 "kern_exec.o", "kern_exec.o.json")

DECL = 'extern void hummer_early_prints (const char *, const char *);'
MARKER_COMMENT = '/* ' + MARKER + ' */\n'

# zzz-boot-progress.patch 落盘后的两个 log 函数体开头（变量 message 形式，
# 正则的字面量分支覆盖不到）
BOOTSTRAP_LOG = ('kernel_bootstrap_log(const char *message)\n'
                 '{\n'
                 '\tboot_progress_stage(message);\n')
BOOTSTRAP_THREAD_LOG = ('kernel_bootstrap_thread_log(const char *message)\n'
                        '{\n'
                        '\tboot_progress_stage(message);\n')
KBOOT_MESSAGE = ('\t{ ' + DECL + ' hummer_early_prints ("kboot", message); }\n')


def kexpand(text):
    """Rewrite every literal boot_progress_stage/process_stage line, pairing
    each with a hummer_early_prints("kboot", ...) call.  Idempotent by line
    shape: our own inserted lines never match the anchored regexes.  The
    vfs_mountroot checkpoints sit inside a double-nested block, so allow
    one or two tabs and keep the original indentation."""
    out = []
    for line in text.splitlines(keepends=True):
        out.append(line)
        m = re.match(r'^(\t+)boot_progress_stage\("([^"\n]+)"\);\n$', line)
        if m:
            out.append(m.group(1) + '{ ' + DECL + ' hummer_early_prints ("kboot", "'
                       + m.group(2) + '"); }\n')
            continue
        m = re.match(
            r'^(\t+)boot_progress_process_stage\(proc_getpid\(p\),p->p_name,'
            r'("([^"\n]+)")\);\n$', line)
        if m:
            out.append(m.group(1) + '{ ' + DECL + ' hummer_early_prints ("kboot", '
                       + m.group(2) + '); }\n')
    return "".join(out)


def stale():
    for path in XNU.rglob("*.o"):
        if path.name in STALE_OBJECTS:
            path.unlink()
            print("kboot-probe: removed stale", path)
        j = path.with_name(path.name + ".json")
        if j.is_file():
            j.unlink()
            print("kboot-probe: removed stale", j)


def main():
    targets = [XNU / "osfmk/kern/startup.c",
               XNU / "bsd/kern/bsd_init.c",
               XNU / "bsd/kern/kern_exec.c"]
    for path in targets:
        if not path.is_file():
            raise SystemExit(f"kboot-probe: {path} missing")
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if MARKER in text:
            print(f"kboot-probe: {path.name} already patched")
            continue

        if path.name == "startup.c":
            for anchor in (BOOTSTRAP_LOG, BOOTSTRAP_THREAD_LOG):
                n = text.count(anchor)
                if n != 1:
                    raise SystemExit(
                        "error: kboot-probe: startup.c: anchor found %d times:\n%s"
                        % (n, anchor))
                text = text.replace(anchor, anchor + KBOOT_MESSAGE)
        else:
            text = kexpand(text)
            if 'hummer_early_prints ("kboot"' not in text:
                raise SystemExit(
                    "error: kboot-probe: %s: no literal stage lines matched"
                    % path.name)

        text = kexpand(text) if path.name == "startup.c" else text
        text = MARKER_COMMENT + text
        path.write_text(text, encoding="utf-8", errors="surrogateescape")
        print("kboot-probe: patched", path)

    stale()
    print("kboot-probe: kernel bootstrap checkpoints now visible on the panel")


if __name__ == "__main__":
    main()

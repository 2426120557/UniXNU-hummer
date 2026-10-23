#!/usr/bin/env python3
"""hummer: put the kernel's own startup checkpoints on the panel ("kboot").

依据（文档 / 上游补丁 / 已验证的探针机制，2026-10-23）：

1. docs/zh/guide/debugging + docs/zh/porting/bring-up：SMP 之后要看内核走到哪，
   官方手段是 boot-progress 诊断页（boot-progress=1/2）。
2. patches/xnu/zzz-boot-progress.patch（自动全量应用）在 startup.c /
   bsd_init.c / kern_exec.c 里插入了 boot_progress_stage 调用，但
   osfmk/kern/boot_progress.h 里 XNU_BOOT_PROGRESS=0 时是空宏，所以面板上看
   不到它们。完整诊断页需要 baseline 的 OWNER_ROOT_AUDIT / NATIVE_VIRTIO_BLOCK
   前置，且 scripts/build-boot-progress.py 与 kits/sheng/BootProgress.c 在公开
   仓库缺失 —— 一次引入太多新变量。
3. 本脚本给每个 stage/process_stage 检查点配一行
   hummer_early_prints("kboot", ...)（渲染器由 build-hummer-console-probe.py
   提供；C 文件里块内 extern 原型合法，clang 拒绝的是 C++ 的块内 extern "C"）。

CI 实测教训（run 39919506734）：baseline 每次运行先对 zzz-boot-progress.patch
做 `git apply --reverse --check` 来判断它是否已应用（build-xnu-4k-baseline.sh
38-47 行），判定为未应用时会重新正向应用并在 --check 上失败退出。因此**只改源
文件是不行的** —— 插入行落在 zzz hunk 的上下文里，反向判定必然失败。

所以本脚本同时改两处，保持二者一致：
  a. 源文件（.cache/xnu-work 下三个 .c）：在 boot_progress_stage 行后插调用；
  b. zzz-boot-progress.patch 本身（tree/patches/xnu/ 是指向 UniXNU checkout
     的符号链接，checkout 可写）：在每个对应的 + 行后面追加同款 kboot + 行，
     并把所在 hunk 的新侧行数加一。这样反向 --check 面对的是"补丁与源文件
     一致"的状态，反向 -> 正向重放后源文件仍是 kboot 状态。

幂等：源文件或补丁文件里已有 MARKER 即跳过。改动让 startup.o / bsd_init.o /
kern_exec.o 过期，STALE_OBJECTS 把它们连同 .o.json 一起删掉强制重编。
"""
import re
from pathlib import Path

MARKER = "hummer: kboot probe"
XNU = Path(".cache/xnu-work")
PATCH = Path("patches/xnu/zzz-boot-progress.patch")

STALE_OBJECTS = ("startup.o", "startup.o.json",
                 "bsd_init.o", "bsd_init.o.json",
                 "kern_exec.o", "kern_exec.o.json",
                 "arm_init.o", "arm_init.o.json")

DECL = 'extern void hummer_early_prints (const char *, const char *);'
KBOOT_MARK = 'hummer_early_prints ("kboot"'

# zzz 落盘后两个 log 函数体开头（变量 message 形式，补丁里也是 + 行）
BOOTSTRAP_LOG = ('kernel_bootstrap_log(const char *message)\n'
                 '{\n'
                 '\tboot_progress_stage(message);\n')
BOOTSTRAP_THREAD_LOG = ('kernel_bootstrap_thread_log(const char *message)\n'
                        '{\n'
                        '\tboot_progress_stage(message);\n')
KBOOT_MESSAGE = ('\t{ ' + DECL + ' hummer_early_prints ("kboot", message); }\n')


def kcall(arg, indent="\t"):
    return (indent + '{ ' + DECL + ' hummer_early_prints ("kboot", ' + arg + '); }\n')


def kexpand(text):
    """Insert a kboot call after every literal boot_progress_stage /
    boot_progress_process_stage line (one or two tabs of indentation both
    occur: the vfs_mountroot checkpoints sit inside a double-nested block)."""
    out = []
    for line in text.splitlines(keepends=True):
        out.append(line)
        m = re.match(r'^(\t+)boot_progress_stage\("([^"\n]+)"\);\n$', line)
        if m:
            out.append(kcall('"%s"' % m.group(2), m.group(1)))
            continue
        m = re.match(
            r'^(\t+)boot_progress_process_stage\(proc_getpid\(p\),p->p_name,'
            r'("([^"\n]+)")\);\n$', line)
        if m:
            out.append(kcall(m.group(2), m.group(1)))
    return "".join(out)


def patch_kexpand(patch_text):
    """Append a kboot + line after every + boot_progress_stage /
    + boot_progress_process_stage line inside the patch, and grow the
    affected hunk's new-side line count.  The #define line that itself calls
    boot_progress_stage("bsd_init: " x) is not a call site and does not
    match (its argument is not a lone string literal)."""
    out = []
    hunk_new_count = None
    for line in patch_text.splitlines(keepends=True):
        out.append(line)
        m = re.match(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@', line)
        if m:
            hunk_new_count = int(m.group(4)) if m.group(4) is not None else 1
            continue
        if hunk_new_count is None or not line.startswith('+'):
            continue
        body = line[1:]
        m = re.match(r'^(\t+)boot_progress_stage\("([^"\n]+)"\);\n$', body)
        if m:
            out.append('+' + kcall('"%s"' % m.group(2), m.group(1)))
            hunk_new_count += 1
            _fix_last_hunk_header(out, hunk_new_count)
            continue
        m = re.match(
            r'^(\t+)boot_progress_process_stage\(proc_getpid\(p\),p->p_name,'
            r'("([^"\n]+)")\);\n$', body)
        if m:
            out.append('+' + kcall(m.group(2), m.group(1)))
            hunk_new_count += 1
            _fix_last_hunk_header(out, hunk_new_count)
            continue
        if body == '\tboot_progress_stage(message);\n':
            out.append('+' + KBOOT_MESSAGE)
            hunk_new_count += 1
            _fix_last_hunk_header(out, hunk_new_count)
    return "".join(out)


def _fix_last_hunk_header(out, new_count):
    """Rewrite the newest @@ header's new-side count in place."""
    for i in range(len(out) - 1, -1, -1):
        m = re.match(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@', out[i])
        if m:
            old = m.group(0)
            oldside = m.group(1) + ("," + m.group(2) if m.group(2) else "")
            newside = m.group(3) + "," + str(new_count)
            out[i] = "@@ -%s +%s @@\n" % (oldside, newside)
            return


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
               XNU / "osfmk/arm/arm_init.c",
               XNU / "bsd/kern/bsd_init.c",
               XNU / "bsd/kern/kern_exec.c"]
    patched_any = False
    for path in targets:
        if not path.is_file():
            raise SystemExit(f"kboot-probe: {path} missing")
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if KBOOT_MARK in text:
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
            text = kexpand(text)
        else:
            text = kexpand(text)
            if KBOOT_MARK not in text:
                raise SystemExit(
                    "error: kboot-probe: %s: no literal stage lines matched"
                    % path.name)
        # 注意：不要在文件头插 MARKER 注释 —— zzz 补丁的第一个 hunk 在文件头
        # （@@ -1,3 +1,4 @@ 加 include），任何头部插入都会破坏反向应用的上下文。
        # newline="" 与 armcpu 探针同理：Windows 上禁止 LF -> CRLF 转换，
        # 否则 git apply 的文本匹配全盘失效。
        with open(path, "w", encoding="utf-8", errors="surrogateescape",
                  newline="") as handle:
            handle.write(text)
        patched_any = True
        print("kboot-probe: patched", path)

    # 同步补丁文件本身：tree/patches/xnu 是指向 UniXNU checkout 的符号链接，
    # 写穿透的符号链接即可更新模块源文件。
    if PATCH.is_file() and 'hummer_early_prints' not in PATCH.read_text(
            encoding="utf-8", errors="replace"):
        original = PATCH.read_text(encoding="utf-8", errors="surrogateescape")
        updated = patch_kexpand(original)
        if updated == original:
            print("kboot-probe: WARNING: patch file unchanged -- no + stage lines matched")
        else:
            with open(PATCH.resolve(), "w", encoding="utf-8",
                      errors="surrogateescape", newline="") as handle:
                handle.write(updated)
            print("kboot-probe: extended", PATCH.resolve(),
                  "(%d -> %d bytes)" % (len(original), len(updated)))
    else:
        print("kboot-probe: patch file missing or already extended")

    stale()
    print("kboot-probe: kernel bootstrap checkpoints now visible on the panel")


if __name__ == "__main__":
    main()

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
  a. 源文件（.cache/xnu-work 下的 .c）：在锚点行前后插 kboot 调用；
  b. 被双改写的补丁文件本身（tree/patches/xnu/ 是指向 UniXNU checkout 的符号
     链接，checkout 可写）：在每个对应的 + 行前后追加同款 kboot + 行，并把所
     在 hunk 的新侧行数加一。这样反向 --check 面对的是"补丁与源文件一致"的
     状态，反向 -> 正向重放后源文件仍是 kboot 状态。
     只有锚点行本身落在某个补丁的 + 行上才需要双改写；锚点在补丁 hunk 上下文
     （空格行）里的插入是禁止的 —— 那会破坏该补丁的反向判定，且无法通过改补
     弥补（上下文行不是我们产出的）。v53 的每个锚点都核对过：要么是 zzz 的
     + 行（双改写），要么不在任何补丁的 hunk 里（纯源码插入）。

v51 教训（正则盲区）：kexpand 只匹配字面量字符串参数，而 zzz 的
"exec process signature REFUSED/accepted" 一行是三元表达式 —— 没有生成 kboot
行且 boot_progress 宏为空 ⇒ v51 真机面板停在 enter 不代表挂死，结果行根本没
被打印。v53 把整个 exec 尾段一次性盖满（board.toml 同轮加 cs_debug=9）：

  H2/H1/H3 判别矩阵（v53 真机照片读法）：
  - "psig task enter" 后无 "psig queries done"
      -> 挂死在 AMFI entitlement 查询（IOTaskHasEntitlement）内部。
  - "psig queries done" 有、"psig err" 无
      -> 挂死在 __ARM_KERNEL_PROTECT__ 分支 / 返回路径（几乎不可能）。
  - "kboot psig err" != 0 -> 签名被拒，error 即拒绝码（EACCES=13 等）。
  - "kboot tps kr" != 0 -> thread_process_signature 拒绝（EXEC_EXIT_REASON_MACHINE_THREAD）。
  - "kboot exec rc" = 0 -> exec 成功；之后若无 "kboot exit"/"kboot uabt esr"
      则 launchd 在用户态安静运行 —— 面板静默 = 可见性问题而非内核问题。
  - "kboot exec rc" != 0 -> exec 失败码；后续应有 SIGKILL 打印与
      "Process 1 exec of ... failed" panic（v44 同款）。
  - "kboot uabt esr" -> launchd/dyld 在用户态异常，ESR/FAR 直接给出现场。
  - "kboot exit <name>" -> 有进程退出（launchd 死了就会看到）。

幂等：源文件或补丁文件里已有对应幂等键即跳过。改动让相关 .o 过期，
STALE_OBJECTS 把它们连同 .o.json 一起删掉强制重编。
"""
import re
from pathlib import Path

MARKER = "hummer: kboot probe"
XNU = Path(".cache/xnu-work")
PATCH = Path("patches/xnu/zzz-boot-progress.patch")
PATCH2 = Path("patches/xnu/native-4k-signature-hash-cache.patch")
# v53：exec init 检查点落在 load_init_program 的 init_programs 循环里，而该循环
# 正是 offline-owner-systemos-mount.patch 第三个 hunk（init_envp 改写）的上下文
# —— 本地全量模拟抓住的：不双改写它，CI pass 2 会在它的反向 check 上误判并
# 硬失败（与 v51 的 zzz 教训同款）。锚点在该补丁里恰好是 + 行，同一机制可用。
PATCH3 = Path("patches/xnu/offline-owner-systemos-mount.patch")

STALE_OBJECTS = ("startup.o", "startup.o.json",
                 "bsd_init.o", "bsd_init.o.json",
                 "kern_exec.o", "kern_exec.o.json",
                 "arm_init.o", "arm_init.o.json",
                 "ubc_subr.o", "ubc_subr.o.json",
                 "machine_task.o", "machine_task.o.json",
                 "sleh.o", "sleh.o.json",
                 "kern_exit.o", "kern_exit.o.json",
                 "debug.o", "debug.o.json",
                 "vfs_syscalls.o", "vfs_syscalls.o.json")

DECL = 'extern void hummer_early_prints (const char *, const char *);'
DECL_U32X = 'extern void hummer_early_u32x (const char *, unsigned int);'
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
    occur: the vfs_mountroot checkpoints sit inside a double-nested block).
    三元表达式行（exec process signature REFUSED/accepted）刻意不在这里处理，
    由 KEXEC_OPS 用值打印覆盖 —— 见模块 docstring 的判别矩阵。"""
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


# v52: exec 签名准备路径的检查点（native-4k-signature-hash-cache.patch 的
# 调用点）。补丁侧在对应 + 行前后插 kboot + 行；源码侧在 ubc_subr.c 的
# 同一行前后插（两侧行集合保持一致，反向 check 才能通过）。
N4K_CALL = '\terror = cs_native4k_prepare(vp, &tmp_blob);\n'
N4K_ENTER = '\t{ ' + DECL + ' hummer_early_prints ("kboot", "exec native4k prepare"); }\n'
N4K_RC = '\t{ ' + DECL_U32X + ' hummer_early_u32x ("kboot n4k rc", (unsigned int) error); }\n'
PATCH2_OPS = {
    "file": "bsd/kern/ubc_subr.c",
    "anchor": '+' + N4K_CALL,
    "before": ['+' + N4K_ENTER],
    "after": ['+' + N4K_RC],
}


def apply_ops_to_patch(patch_text, ops_list):
    """Insert ops' before/after lines around each op's anchor inside the
    op's file section of the patch, growing that hunk's new-side count.
    Each op must fire exactly once; a missing anchor is a hard error."""
    out = []
    current_file = None
    hunk_new_count = None
    pending = list(ops_list)
    for line in patch_text.splitlines(keepends=True):
        m = re.match(r'^\+\+\+ b/(\S+)', line)
        if m:
            current_file = m.group(1)
            out.append(line)
            continue
        m = re.match(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@', line)
        if m:
            hunk_new_count = int(m.group(4)) if m.group(4) is not None else 1
            out.append(line)
            continue
        fired = None
        for op in pending:
            if (current_file == op["file"] and hunk_new_count is not None
                    and line == op["anchor"]):
                fired = op
                break
        if fired is not None:
            for extra in fired.get("before", []):
                out.append(extra)
                hunk_new_count += 1
            out.append(line)
            for extra in fired.get("after", []):
                out.append(extra)
                hunk_new_count += 1
            pending.remove(fired)
            _fix_last_hunk_header(out, hunk_new_count)
            continue
        out.append(line)
    if pending:
        raise SystemExit(
            "error: kboot-probe: patch ops anchors not found: %s"
            % ", ".join(op["file"] + ":" + op["anchor"][:60] for op in pending))
    return "".join(out)


def apply_ops_to_source(text, ops_list, filename):
    """Insert before/after lines around each anchor in a source file.
    Every anchor must occur exactly once (count != 1 is a hard error so a
    drifted tree fails loudly instead of silently mis-instrumenting)."""
    for op in ops_list:
        n = text.count(op["anchor"])
        if n != 1:
            raise SystemExit(
                "error: kboot-probe: %s: anchor found %d times:\n%s"
                % (filename, n, op["anchor"]))
        text = text.replace(
            op["anchor"],
            "".join(op.get("before", [])) + op["anchor"]
            + "".join(op.get("after", [])))
    return text


# ---------------------------------------------------------------------------
# v53：exec 尾段 + 首个用户进程的完整覆盖。
#
# 锚点分类（见模块 docstring 的双改写规则）：
#   - kern_exec.c 三元结果行：zzz 的 + 行 -> 双改写（ZZZ_OPS + KEXEC_OPS 同锚）。
#   - thread_process_signature / exec init 调用：不在任何补丁 hunk 里
#     （offline-owner-* / zzy / zzz 的 kern_exec.c hunk 都在 load_init_program
#     前段与 process_signature 段，与这两个锚点隔了多行纯上下文）——纯源码插入，
#     本地全量 53 补丁反向 check 验证。
#   - machine_task.c：没有任何补丁碰过它（grep patches/xnu 确认）——纯源码插入。
#   - sleh.c / kern_exit.c 三个锚点：zzz 的 + 行 -> 双改写。
KEXEC_TERNARY = ('\t\tboot_progress_process_stage(proc_getpid(p),p->p_name,'
                 'error ? "exec process signature REFUSED" : '
                 '"exec process signature accepted");\n')
KEXEC_TERNARY_KCALL = ('\t\t{ ' + DECL_U32X + ' hummer_early_u32x '
                       '("kboot psig err", (unsigned int) error); }\n')

TPS_CALL = '\t\tkr = thread_process_signature(imgp->ip_new_thread, proc_get_task_raw(p));\n'

EXEC_INIT_CALL = ('\t\terror = load_init_program_at_path(p, '
                  '(user_addr_t)scratch_addr, init_programs[i], init_envp);\n')

# v68：launchd 之后的用户态可见性。
# 1) 真实 execve 探针（kern_exec.c __mac_execve，xnu-10063.141.1 中 5165..5481
#    —— 四个碰 kern_exec.c 的补丁（zzz / systemos-mount / z-air-init /
#    gpu-addon-mount）的 hunk 全部落在 1..7436 的其它区间，与该锚点无重叠，
#    纯源码插入；锚点唯一性在 141.1 原始源码上核对过 = 1）。
#    exec_resettextvp 已把 p->p_name 换成新程序名（kern_exec.c exec_resettextvp
#    里 bcopy cn_nameptr），所以这里打出来的是 exec 之后的新名字。静态计数器
#    封顶 16 条，防止 launchd 起一堆服务把面板刷爆。
EXECVE_ANCHOR = ('\t\tif (!error) {\n'
                 '\t\t\ttask_transfer_turnstile_watchports(old_task,\n')
EXECVE_OP = {"anchor": EXECVE_ANCHOR,
             "before": ['\t\t{ static int hk_exec_n = 0; ' + DECL +
                        ' if (hk_exec_n < 16) { ++hk_exec_n; '
                        'hummer_early_prints ("kboot execve", p->p_name); } }\n']}

KEXEC_OPS = [
    {"anchor": KEXEC_TERNARY, "after": [KEXEC_TERNARY_KCALL]},
    {"anchor": TPS_CALL,
     "before": ['\t\t{ ' + DECL + ' hummer_early_prints ("kboot", "exec thread psig"); }\n'],
     "after": ['\t\t{ ' + DECL_U32X + ' hummer_early_u32x ("kboot tps kr", (unsigned int) kr); }\n']},
    {"anchor": EXEC_INIT_CALL,
     "before": ['\t\t{ ' + DECL + ' hummer_early_prints ("kboot", "exec init"); }\n'],
     "after": ['\t\t{ ' + DECL_U32X + ' hummer_early_u32x ("kboot exec rc", (unsigned int) error); }\n']},
    dict(EXECVE_OP),  # v68：真实 execve（前 16 个，见 EXECVE_ANCHOR 注释）
]

MACHINE_TASK_ENTER = '\tbool const x18_entitled =\n'
MACHINE_TASK_UESC = '\t    IOTaskHasEntitlement(task, "com.apple.private.uexc");\n'
MACHINE_TASK_ENDIF = '#endif /* !__ARM_KERNEL_PROTECT__ */\n'
MACHINE_TASK_OPS = [
    {"anchor": MACHINE_TASK_ENTER,
     "before": ['\t{ ' + DECL + ' hummer_early_prints ("kboot", "psig task enter"); }\n']},
    {"anchor": MACHINE_TASK_UESC,
     "after": ['\t{ ' + DECL + ' hummer_early_prints ("kboot", "psig queries done"); }\n']},
    {"anchor": MACHINE_TASK_ENDIF,
     "after": ['\t{ ' + DECL_U32X + ' hummer_early_u32x ("kboot psig kr", (unsigned int) kr); }\n']},
]

SLEH_UBRK = '\tboot_progress_user_breakpoint(state, esr);\n'
SLEH_UABT = '\tboot_progress_user_abort(state, esr, fault_addr);\n'
SLEH_OPS = [
    {"anchor": SLEH_UBRK,
     "after": ['\t{ ' + DECL_U32X + ' hummer_early_u32x ("kboot ubrk esr", esr); }\n']},
    {"anchor": SLEH_UABT,
     "after": ['\t{ ' + DECL_U32X + ' hummer_early_u32x ("kboot uabt esr", esr); }\n',
               '\t{ ' + DECL_U32X + ' hummer_early_u32x ("kboot uabt far", (unsigned int) fault_addr); }\n']},
]

KEXIT_OSRCODE = '\t    p->p_exit_reason ? p->p_exit_reason->osr_code : 0);\n'
KEXIT_OPS = [
    {"anchor": KEXIT_OSRCODE,
     "after": ['\t{ ' + DECL + ' hummer_early_prints ("kboot exit", p->p_name); }\n',
               '\t{ ' + DECL_U32X + ' hummer_early_u32x ("kboot exit rv", (unsigned int) rv); }\n']},
]

# v73：panic 从来没在面板上出现过。
# 面板上的每一行都来自探针显式调用的 hummer_early_prints；panic() 走的是
# kprintf（串口/控制台），跟这个自绘渲染器毫无关系。所以 v71/v72 真机上
# "kboot exit init / rv=0 之后什么都没有" 有两种完全不同的解释：内核 panic 了
# 但看不见，或者 launchd 活着但卡住。分辨这两者必须先把 panic 印到面板上。
#
# panic、panic_with_options、panic_with_data 三个变体全部汇聚到
# panic_trap_to_debugger（debug.c:1132），所以只在这一处的函数体开头插一次，
# 就能覆盖全部。打三行：
#   PANIC <格式串>        —— 格式串本身就能区分大多数死点
#   PANIC caller lo/hi    —— 调用者地址：内核按固定基址链接
#                            （ARM64_LINK_ADDR=0xfffffff007004000，slide -0x1000000），
#                            事后能用 llvm-nm 对着 kernel.macho 反查是哪个 panic 调用点。
# 没有任何引擎补丁碰 osfmk/kern/debug.c（grep patches/xnu 全部补丁的 --- a/ 清单核实），
# 所以这是纯源码插入，不涉及 zzz 那种双改写。
PANIC_ANCHOR = ('panic_trap_to_debugger(const char *panic_format_str, va_list *panic_args, '
                'unsigned int reason, void *ctx,\n'
                '    uint64_t panic_options_mask, void *panic_data_ptr, unsigned long panic_caller)\n'
                '{\n')
PANIC_OPS = [
    {"anchor": PANIC_ANCHOR,
     "after": ['\t{ ' + DECL + ' hummer_early_prints ("PANIC", panic_format_str); }\n',
               '\t{ ' + DECL_U32X + ' hummer_early_u32x ("PANIC caller lo", (unsigned int) panic_caller); }\n',
               '\t{ ' + DECL_U32X + ' hummer_early_u32x ("PANIC caller hi", '
               '(unsigned int) ((unsigned long long) panic_caller >> 32)); }\n']},
]

# v68：launchd 之后的用户态可见性。
# 1) 通用挂载探针（bsd/vfs/vfs_syscalls.c，没有任何补丁碰它 —— 纯源码插入，
#    2026-10-24 对照引擎 b6ae99c 的 patches/xnu 全部 51 个补丁的 --- a/ 清单核实）。
#    mount_common 是 mount() 系统调用和内核挂载（KERNEL_MOUNT_KMOUNT）的共同
#    收口：launchd 挂 Data 卷、SystemOS addon、GPU addon 都会走这里。
#    入口打 fstypename（"apfs" 等），出口打返回码。
MOUNT_ENTER_ANCHOR = '\tboolean_t kernelmount = !!(internal_flags & KERNEL_MOUNT_KMOUNT);\n'
MOUNT_EXIT_ANCHOR = ('\tif (vfsp_ref) {\n'
                     '\t\tmount_list_lock();\n'
                     '\t\tvfsp->vfc_refcount--;\n'
                     '\t\tmount_list_unlock();\n'
                     '\t}\n')
MOUNT_OPS = [
    {"anchor": MOUNT_ENTER_ANCHOR,
     "after": ['\t{ ' + DECL + ' hummer_early_prints ("kboot mount", fstypename); }\n']},
    {"anchor": MOUNT_EXIT_ANCHOR,
     "after": ['\t{ ' + DECL_U32X +
               ' hummer_early_u32x ("kboot mount rc", (unsigned int) error); }\n']},
]

ZZZ_OPS = [
    {"file": "bsd/kern/kern_exec.c", "anchor": '+' + KEXEC_TERNARY,
     "after": ['+' + KEXEC_TERNARY_KCALL]},
    {"file": "osfmk/arm64/sleh.c", "anchor": '+' + SLEH_UBRK,
     "after": ['+' + SLEH_OPS[0]["after"][0]]},
    {"file": "osfmk/arm64/sleh.c", "anchor": '+' + SLEH_UABT,
     "after": ['+' + SLEH_OPS[1]["after"][0], '+' + SLEH_OPS[1]["after"][1]]},
    {"file": "bsd/kern/kern_exit.c", "anchor": '+' + KEXIT_OSRCODE,
     "after": ['+' + KEXIT_OPS[0]["after"][0], '+' + KEXIT_OPS[0]["after"][1]]},
]

# v53：exec init 双改写针对的是 systemos-mount 补丁（见 PATCH3 注释）
PATCH3_OPS = {
    "file": "bsd/kern/kern_exec.c",
    "anchor": '+' + EXEC_INIT_CALL,
    "before": ['+' + KEXEC_OPS[2]["before"][0]],
    "after": ['+' + KEXEC_OPS[2]["after"][0]],
}

# 每个源文件的幂等键（v53 新增文件各自独立，避免整树重跑时互相跳过）
IDEMPOTENT_KEYS = {
    "machine_task.c": '"psig queries done"',
    "sleh.c": '"kboot uabt esr"',
    "kern_exit.c": '"kboot exit rv"',
    "kern_exec.c": '"kboot exec rc"',
    "ubc_subr.c": 'kboot n4k rc',
    "vfs_syscalls.c": '"kboot mount rc"',
    "debug.c": '"PANIC caller lo"',
}

SOURCE_OPS = {
    "kern_exec.c": KEXEC_OPS,
    "machine_task.c": MACHINE_TASK_OPS,
    "sleh.c": SLEH_OPS,
    "kern_exit.c": KEXIT_OPS,
    "debug.c": PANIC_OPS,
    "vfs_syscalls.c": MOUNT_OPS,
}


def extend_patch_file(patch_path, transform, label):
    if not patch_path.is_file():
        raise SystemExit(f"kboot-probe: {patch_path} missing")
    text = patch_path.read_text(encoding="utf-8", errors="surrogateescape")
    if 'hummer_early_prints' in text:
        print(f"kboot-probe: {patch_path.name} already extended ({label})")
        return
    updated = transform(text)
    if updated == text:
        print(f"kboot-probe: WARNING: {patch_path.name} unchanged ({label})")
        return
    with open(patch_path.resolve(), "w", encoding="utf-8",
              errors="surrogateescape", newline="") as handle:
        handle.write(updated)
    print("kboot-probe: extended", patch_path.resolve(),
          "(%d -> %d bytes, %s)" % (len(text), len(updated), label))


def main():
    # (路径, 处理方式)；kexpand 只用于 zzz 检查点为字面量的四个老文件
    targets = [
        (XNU / "osfmk/kern/startup.c", "kexpand"),
        (XNU / "osfmk/arm/arm_init.c", "kexpand"),
        (XNU / "bsd/kern/bsd_init.c", "kexpand"),
        (XNU / "bsd/kern/kern_exec.c", "kexpand+ops"),
        (XNU / "bsd/kern/ubc_subr.c", "n4k"),
        (XNU / "osfmk/arm64/machine_task.c", "ops"),
        (XNU / "osfmk/arm64/sleh.c", "ops"),
        (XNU / "bsd/kern/kern_exit.c", "ops"),
        (XNU / "osfmk/kern/debug.c", "ops"),  # v73：panic -> 面板
        (XNU / "bsd/vfs/vfs_syscalls.c", "ops"),  # v68：mount_common
    ]
    for path, mode in targets:
        if not path.is_file():
            raise SystemExit(f"kboot-probe: {path} missing")
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        key = IDEMPOTENT_KEYS.get(path.name, KBOOT_MARK)
        if key in text:
            print(f"kboot-probe: {path.name} already patched")
            continue
        if mode == "kexpand":
            if path.name == "startup.c":
                for anchor in (BOOTSTRAP_LOG, BOOTSTRAP_THREAD_LOG):
                    n = text.count(anchor)
                    if n != 1:
                        raise SystemExit(
                            "error: kboot-probe: startup.c: anchor found %d times:\n%s"
                            % (n, anchor))
                    text = text.replace(anchor, anchor + KBOOT_MESSAGE)
            text = kexpand(text)
            if KBOOT_MARK not in text:
                raise SystemExit(
                    "error: kboot-probe: %s: no literal stage lines matched"
                    % path.name)
        elif mode == "kexpand+ops":
            text = kexpand(text)
            text = apply_ops_to_source(text, SOURCE_OPS[path.name], path.name)
            if KBOOT_MARK not in text or key not in text:
                raise SystemExit(
                    "error: kboot-probe: %s: instrumentation did not land"
                    % path.name)
        elif mode == "n4k":
            n = text.count(N4K_CALL)
            if n != 1:
                raise SystemExit(
                    "error: kboot-probe: ubc_subr.c: cs_native4k_prepare "
                    "call site found %d times" % n)
            text = text.replace(N4K_CALL, N4K_ENTER + N4K_CALL + N4K_RC)
        elif mode == "ops":
            text = apply_ops_to_source(text, SOURCE_OPS[path.name], path.name)
            if key not in text:
                raise SystemExit(
                    "error: kboot-probe: %s: instrumentation did not land"
                    % path.name)
        # 注意：不要在文件头插 MARKER 注释 —— zzz 补丁的第一个 hunk 在文件头
        # （@@ -1,3 +1,4 @@ 加 include），任何头部插入都会破坏反向应用的上下文。
        # newline="" 与 armcpu 探针同理：Windows 上禁止 LF -> CRLF 转换，
        # 否则 git apply 的文本匹配全盘失效。
        with open(path, "w", encoding="utf-8", errors="surrogateescape",
                  newline="") as handle:
            handle.write(text)
        print("kboot-probe: patched", path)

    # 同步补丁文件本身：tree/patches/xnu 是指向 UniXNU checkout 的符号链接，
    # 写穿透的符号链接即可更新模块源文件。
    def patch_transform(text):
        text = patch_kexpand(text)
        return apply_ops_to_patch(text, ZZZ_OPS)
    extend_patch_file(PATCH, patch_transform, "zzz stage lines + v53 ops")

    # v52: exec 签名验证（native-4k-signature-hash-cache.patch 的
    # cs_native4k_prepare）—— 调用点 enter 行与返回码打印，同一套双改写机制。
    extend_patch_file(PATCH2, lambda text: apply_ops_to_patch(text, [PATCH2_OPS]),
                      "native4k prepare ops")

    # v53: exec init 检查点在 systemos-mount 补丁 hunk 的上下文里 —— 第三份双改写。
    extend_patch_file(PATCH3, lambda text: apply_ops_to_patch(text, [PATCH3_OPS]),
                      "systemos exec init ops")

    stale()
    print("kboot-probe: exec tail + first user process checkpoints on the panel")


if __name__ == "__main__":
    main()

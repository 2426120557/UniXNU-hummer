// SPDX-License-Identifier: BSD-2-Clause-Patent
// Root queue takeover; shared with the host fault-injection harness.
static bool hostWaitRegister (unsigned offset, uint32_t mask, uint32_t value, unsigned timeoutMs = 100)
{
  uint64_t deadline;
  clock_interval_to_deadline (timeoutMs, kMillisecondScale, &deadline);
  do {
    if ((read32 (gHost.hc, offset) & mask) == value) return true;
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
    // Early boot has no scheduler wakeup guarantee for IODelay.
#if defined(__aarch64__)
    __asm__ volatile ("yield");
#endif
#else
    IODelay (10);
#endif
  } while (mach_absolute_time () < deadline);
  return false;
}

static bool hostStopQueue (const char **why)
{
  // A legacy UTRL cannot take over an active UFS 4.x MCQ engine.
  if ((read32 (gHost.hc, 0x00) & 0x40000000U) && (read32 (gHost.hc, 0x300) & 1)) {
    *why = "MCQ active; legacy takeover refused";
    return false;
  }
  // Confirm the firmware has no active transfer/task before changing its
  // request list or the storage stream's SMMU routing.
  if (!(read32 (gHost.hc, 0x34) & 1) || (read32 (gHost.hc, 0x30) & 0x0f) != 0x0f) {
    recordTransport (kIOReturnNotReady, 15);
    *why = "controller not ready for takeover";
    return false;
  }
  if (read32 (gHost.hc, 0x58) || read32 (gHost.hc, 0x78)) {
    recordTransport (kIOReturnBusy, 15);
    *why = "firmware request outstanding";
    return false;
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    recordTransport (kIOReturnIOError, 15);
    *why = "firmware controller error";
    return false;
  }
  hostWrite (0x24, 0);
  hostWrite (0x60, 0);
  if (!hostWaitRegister (0x60, 1, 0)) {
    recordTransport (kIOReturnTimeout, 15);
    *why = "request list stop";
    return false;
  }
  return true;
}

#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
static bool hostLinkCommand (uint32_t command, uint32_t argument, const char **why)
{
  gRootDiagnostic.linkCommand = command;
  gRootDiagnostic.linkResult = 0xffffffff;
  gRootDiagnostic.linkStatus = read32 (gHost.hc, 0x20);
  if (!hostWaitRegister (0x30, 8, 8)) {
    recordTransport (kIOReturnTimeout, 15);
    *why = "UIC command not ready";
    return false;
  }
  const uint32_t completion = command == 0x18 ? 0x420 : 0x400;
  hostWrite (0x20, completion); // Clear stale completion, preserving error bits.
  hostWrite (0x94, argument);
  hostWrite (0x98, 0);
  hostWrite (0x9c, 0);
  hostWrite (0x90, command);
  bool done = hostWaitRegister (0x20, 0x400, 0x400);
  gRootDiagnostic.linkResult = read32 (gHost.hc, 0x98) & 0xff;
  // UHXS may arrive after UCCS; allow the link transition its own bounded wait.
  if (done && !gRootDiagnostic.linkResult && command == 0x18 &&
      !(read32 (gHost.hc, 0x20) & (kUfsFatalStatus | 4))) {
    done = hostWaitRegister (0x20, 0x20, 0x20, 500);
  }
  gRootDiagnostic.linkStatus = read32 (gHost.hc, 0x20);
  if (!done || gRootDiagnostic.linkResult || (gRootDiagnostic.linkStatus & (kUfsFatalStatus | 4)) ||
      (command == 0x18 && ((read32 (gHost.hc, 0x30) >> 8) & 7) != 1)) {
    recordTransport (done ? kIOReturnIOError : kIOReturnTimeout, 15);
    *why = "UIC link command failed";
    return false;
  }
  hostWrite (0x20, completion);
  return true;
}

static bool hostWakeLink (const char **why)
{
  gRootDiagnostic.stage = "link state";
  // Keep the link awake between the state query and the first transfer.
  gRootDiagnostic.linkAhit = read32 (gHost.hc, 0x18);
  if (gRootDiagnostic.linkAhit) hostWrite (0x18, 0);
  if (read32 (gHost.hc, 0x18)) {
    recordTransport (kIOReturnIOError, 15);
    *why = "auto hibernation disable";
    return false;
  }
  // Local MPHY_TX_FSM_STATE, TX lane 0 (same selector as ufs-qcom).
  if (!hostLinkCommand (0x01, 0x00410000, why)) return false;
  gRootDiagnostic.txBefore = gRootDiagnostic.txAfter = read32 (gHost.hc, 0x9c);
  if (gRootDiagnostic.txBefore != 1) return true;
  gRootDiagnostic.stage = "Hibern8 exit";
  gRootDiagnostic.txAfter = 0xffffffff;
  if (!hostLinkCommand (0x18, 0, why) || !hostLinkCommand (0x01, 0x00410000, why)) return false;
  gRootDiagnostic.txAfter = read32 (gHost.hc, 0x9c);
  if (gRootDiagnostic.txAfter == 1) {
    recordTransport (kIOReturnNotReady, 15);
    *why = "link remains in Hibern8";
    return false;
  }
  return true;
}
#endif

static bool hostStartQueue (const char **why)
{
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
  if (!hostWakeLink (why)) return false;
  // Single-slot polling must not inherit firmware's completion aggregation policy.
  gRootDiagnostic.iacrBefore = read32 (gHost.hc, 0x4c);
  hostWrite (0x4c, 0);
  gRootDiagnostic.iacr = read32 (gHost.hc, 0x4c);
  if (gRootDiagnostic.iacr & 0x80000000U) {
    recordTransport (kIOReturnIOError, 15);
    *why = "interrupt aggregation disable";
    return false;
  }
#endif
  hostWrite (0x50, static_cast<uint32_t> (gHost.phys));
  hostWrite (0x54, static_cast<uint32_t> (gHost.phys >> 32));
  if (read32 (gHost.hc, 0x50) != uint32_t (gHost.phys) ||
      read32 (gHost.hc, 0x54) != uint32_t (gHost.phys >> 32)) {
    recordTransport (kIOReturnIOError, 15);
    *why = "request list address";
    return false;
  }
  gRootDiagnostic.isBefore = read32 (gHost.hc, 0x20);
  hostWrite (0x20, gRootDiagnostic.isBefore);
  hostWrite (0x60, 1);
  if (!hostWaitRegister (0x60, 1, 1) || !hostWaitRegister (0x30, 3, 3)) {
    recordTransport (kIOReturnTimeout, 15);
    *why = "request list ready";
    return false;
  }
  return true;
}

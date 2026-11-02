// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../TransferPoll.h"

struct Sample { uint32_t db, ocs, is; };
struct FakePort {
  std::vector<Sample> samples;
  unsigned index = 0, reads = 0;
  uint32_t doorbell() { return samples[index].db; }
  uint32_t interrupt() { return samples[index].is; }
  uint32_t completedOcs() {
    assert(!(samples[index].db & 1)); // Never inspect a descriptor still owned by HC.
    ++reads;
    return samples[index].ocs;
  }
  bool expired() { return index + 1 == samples.size(); }
  void delay() { assert(!expired()); ++index; }
};

struct PreemptedPort : FakePort {
  bool expired() { assert(index == 0 && samples.size() == 2); ++index; return true; }
  void delay() { assert(false); }
};

int main()
{
  // Exact IMG_2971 observation must not be treated as a completed command.
  FakePort noAcceptance{{{0, 15, 0}, {0, 15, 0}, {0, 15, 0}}};
  UfsTransferPoll a;
  assert(pollUfsTransfer(noAcceptance, a) == UfsTransferEnd::Timeout);
  assert(a.polls == 4 && a.ocs == 15 && a.doorbellSeen == 0);

  FakePort delayed{{{0, 15, 0}, {1, 15, 0}, {0, 15, 0}, {0, 0, 1}}};
  UfsTransferPoll b;
  assert(pollUfsTransfer(delayed, b) == UfsTransferEnd::Completed);
  assert(b.polls == 4 && b.ocs == 0 && b.doorbellSeen == 1 && delayed.reads == 3);

  FakePort immediate{{{0, 0, 0}}};
  UfsTransferPoll c;
  assert(pollUfsTransfer(immediate, c) == UfsTransferEnd::Completed);
  assert(c.doorbellSeen == 0); // Seeing DB=1 must not be mandatory.

  // The controller completes while software is descheduled after sampling DB=1.
  PreemptedPort preempted;
  preempted.samples = {{1, 15, 0}, {0, 0, 1}};
  UfsTransferPoll resumed;
  assert(pollUfsTransfer(preempted, resumed) == UfsTransferEnd::Completed);
  assert(resumed.polls == 2 && resumed.ocs == 0 && resumed.interruptSeen == 1);

  PreemptedPort stillOwned;
  stillOwned.samples = {{1, 15, 0}, {1, 0, 1}};
  UfsTransferPoll owned;
  assert(pollUfsTransfer(stillOwned, owned) == UfsTransferEnd::Timeout && stillOwned.reads == 0);

  PreemptedPort lateFatal;
  lateFatal.samples = {{1, 15, 0}, {0, 0, 0x20000}};
  UfsTransferPoll interrupted;
  assert(pollUfsTransfer(lateFatal, interrupted) == UfsTransferEnd::Fatal && lateFatal.reads == 0);

  FakePort failedCommand{{{1, 15, 0}, {0, 4, 1}}};
  UfsTransferPoll d;
  assert(pollUfsTransfer(failedCommand, d) == UfsTransferEnd::Completed && d.ocs == 4);

  FakePort stuck{{{1, 15, 0}, {1, 15, 0}}};
  UfsTransferPoll e;
  assert(pollUfsTransfer(stuck, e) == UfsTransferEnd::Timeout && stuck.reads == 0);

  for (unsigned bit = 0; bit < 32; ++bit) {
    uint32_t mask = uint32_t(1) << bit;
    if (!(mask & kUfsFatalStatus)) continue;
    FakePort fatal{{{0, 0, mask}}};
    UfsTransferPoll f;
    assert(pollUfsTransfer(fatal, f) == UfsTransferEnd::Fatal);
    assert(fatal.reads == 0 && f.interruptSeen == mask); // Fatal wins over OCS=0.
  }
  puts("PASS: unaccepted, delayed, immediate, preempted, failed, stuck and fatal UFS completions");
}

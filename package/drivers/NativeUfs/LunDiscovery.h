// SPDX-License-Identifier: BSD-2-Clause-Patent
// READ CAPACITY(10), with bounded retries for unit attention after device wake.
struct UfsLun { uint32_t last = 0, blockBytes = 0; };
static UfsLun gLuns[8];
static bool hostDiscoverLuns(const char **why)
{
  for (unsigned lun = 0; lun < 8; ++lun) {
    gLuns[lun] = {};
    uint8_t cdb[10] = { 0x25 };
    uint32_t scsi = 0;
    IOReturn result = kIOReturnNotReady;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      result = hostScsi(lun, cdb, sizeof(cdb), true, 8, 200000, &scsi);
      if (result == kIOReturnSuccess || gHost.poisoned || (gRootDiagnostic.sense >> 16) != 6) break;
    }
    if (gHost.poisoned) { *why = "LUN discovery transport failure"; return false; }
    if (result != kIOReturnSuccess) continue;
    const uint8_t *data = gHost.virt + kDataOffset;
    gLuns[lun] = { loadBe32(data), loadBe32(data + 4) };
  }
  if (!gLuns[0].last || gLuns[0].last == UINT32_MAX || gLuns[0].blockBytes != 4096) {
    *why = "LUN 0 requires 4K blocks and READ(10) capacity";
    return false;
  }
  return true;
}

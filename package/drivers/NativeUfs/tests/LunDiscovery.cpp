// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnNotReady=1;
constexpr unsigned kDataOffset=0;
static uint8_t data[8];
static struct { uint8_t *virt=data; bool poisoned=false; } gHost;
static struct { uint32_t sense=0; } gRootDiagnostic;
static unsigned tries[8], blockBytes=4096;
static bool fault=false, missing=false;
static void be(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=v>>(24-i*8); }
static uint32_t loadBe32(const uint8_t *p) { return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3]; }
static IOReturn hostScsi(uint8_t lun,const uint8_t *cdb,unsigned length,bool read,unsigned bytes,unsigned,uint32_t *) {
  assert(lun<8 && cdb[0]==0x25 && length==10 && read && bytes==8);
  ++tries[lun]; gRootDiagnostic.sense=0;
  if(fault) { gHost.poisoned=true; return 1; }
  if(lun==0 && tries[0]<3) { gRootDiagnostic.sense=0x062900; return 1; }
  if(lun==6 || lun==7 || (missing && lun==0)) return 1;
  be(data,61706239);be(data+4,blockBytes);return 0;
}
#include "../LunDiscovery.h"
static void reset() { memset(tries,0,sizeof(tries));gHost.poisoned=false;fault=missing=false;blockBytes=4096; }
int main() {
  const char *why=nullptr;
  reset(); assert(hostDiscoverLuns(&why));
  assert(tries[0]==3 && gLuns[0].blockBytes==4096 && gLuns[5].last==61706239 && !gLuns[6].blockBytes);
  reset();blockBytes=512;assert(!hostDiscoverLuns(&why));
  reset();missing=true;assert(!hostDiscoverLuns(&why));
  reset();fault=true;assert(!hostDiscoverLuns(&why) && tries[0]==1 && !tries[1]);
  puts("PASS: LUN discovery, unit-attention retry, missing LUN, sector mismatch, poisoned transport");
}

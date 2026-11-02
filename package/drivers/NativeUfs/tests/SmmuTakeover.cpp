// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <array>
#include <vector>
#include <utility>
#include "../SmmuTakeover.h"
int main()
{
  std::array<uint32_t,kUfsSmmuMapBytes/4> r{};
  auto reset=[&] {r.fill(0); r[0x20/4]=0x40000080; r[0x24/4]=0x60000046; r[0x28/4]=0x5111;};
  auto plan=[&] {return planUfsSmmu(0x60,0x8cb5e9000ULL,0x22000,[&](unsigned at){return r.at(at/4);});};
  reset(); auto p=plan(); assert(!p.error && !p.reuse && p.slot==127 && p.bank==69);
  r[(0x800+4*110)/4]=0x80000060; r[(0xc00+4*110)/4]=69;
  r[(0x1000+4*69)/4]=0x10000;
  p=plan(); assert(!p.error && p.reuse && p.slot==110);
  r[(0x800+4*111)/4]=0x80000060; r[(0xc00+4*111)/4]=69; assert(plan().error);
  // Another stream still pointing at the top bank (a mapping left by an earlier run): the plan moves to the next free bank down.
  reset(); r[0x800/4]=0x80000061; r[0xc00/4]=69; p=plan(); assert(!p.error && !p.reuse && p.bank==68 && p.slot==127);
  // The next banks may be held too, or translating: each is skipped; with every attempt blocked the first refusal is reported.
  reset(); r[0x800/4]=0x80000061; r[0xc00/4]=69; r[0x804/4]=0x80000062; r[0xc04/4]=68; r[(128+67)*4096/4]=1; p=plan(); assert(!p.error && p.bank==66);
  reset(); for(unsigned k=0;k<8;++k){r[(0x800+4*k)/4]=0x80000100+k;r[(0xc00+4*k)/4]=69-k;}
  p=plan(); assert(p.error && !__builtin_strcmp(p.error,"reserved context bank already used") && p.bank==69);
  reset(); r[0x800/4]=0x80000060; r[0xc00/4]=1; assert(plan().error);
  // Our stream already mapped to the fallback bank by an earlier run (the top bank held by another stream): reused, not refused.
  reset(); r[0x800/4]=0x80000061; r[0xc00/4]=69; r[0x804/4]=0x80000060; r[0xc04/4]=68; r[(0x1000+4*68)/4]=0x10000;
  p=plan(); assert(!p.error && p.reuse && p.bank==68 && p.slot==1);
  reset(); r[0x800/4]=0x80010060; r[0xc00/4]=69; assert(plan().error);
  reset(); r[0x800/4]=0x80008060; r[0xc00/4]=1; assert(!plan().error); // Distinct 16-bit SID.
  reset(); r[(128+69)*4096/4]=1; p=plan(); assert(!p.error && p.bank==68); // translating top bank: use the next one
  reset(); for(unsigned k=0;k<8;++k) r[(128+69-k)*4096/4]=1; assert(plan().error);
  reset(); r[0x24/4]|=0x80000000; assert(plan().error); // Never guess 64K register geometry.
  reset(); for(unsigned i=0;i<128;++i) {r[(0x800+4*i)/4]=0x80000100+i;r[(0xc00+4*i)/4]=1;}
  assert(plan().error);
  // The reported DMA range fits every 36-bit IDR2 limit, but the previous
  // CBA2R=0 setup faults on its first byte (ARM IHI0062D.c table 3-1).
  reset(); p=plan(); assert(!p.error && p.pages==128);
  auto read=[&](unsigned at){
    assert(!(at>=0xa00 && at<0xc00) && !(at>=0xe00 && at<0x1000));
    return r.at(at/4);
  };
  std::vector<std::pair<unsigned,uint32_t>> writes;
  unsigned drop=0xffffffffU;
  auto write=[&](unsigned at,uint32_t value) {
    writes.emplace_back(at,value);
    if(at!=drop) r.at(at/4)=value;
    if(at==0x800+4*p.slot || at==0xc00+4*p.slot) {
      assert(read(0x1800+4*p.bank)==1);
      assert(read(0x1000+4*p.bank)==0x10000);
      assert(read((p.pages+p.bank)*4096)==0);
    }
  };
  auto configure=[&]{return configureUfsSmmu(p,0x60,read,write);};
  auto inputFault=[&](uint64_t address) {
    // Independent bypass-only architecture rule, not a hardware emulator.
    return !(read(0x1800+4*p.bank)&1) && (address>>32);
  };
  assert(inputFault(0x8cb5e9000ULL));
  assert(!inputFault(0xffffffffULL) && inputFault(0x100000000ULL));
  assert(!configure() && !inputFault(0x8cb5e9000ULL));
  assert(!inputFault(0x8cb60afffULL)); // Final byte of the same allocation.
  assert(writes.size()==5);
  p=plan(); writes.clear(); assert(p.reuse && !configure() && writes.empty());
  r[(0x1800+4*p.bank)/4]=0;
  assert(!configure() && writes.size()==1 && writes[0].first==0x1800+4*p.bank);
  // A refused context-format write must stop before stream publication.
  for(unsigned offset : {0x1800+4*69, 0x1000+4*69, 0xc00+4*127, 0x800+4*127}) {
    reset(); p=plan(); writes.clear(); drop=offset;
    assert(configure());
    if(offset!=0x800+4*127) assert(read(0x800+4*127)==0);
  }
  reset(); p=plan(); r[(128+69)*4096/4]=2; drop=(128+69)*4096;
  assert(configure()); // An ignored disable write cannot publish a stream.
  assert(read(0x800+4*p.slot)==0);
  // Full extent, overflow and each independent hardware address width.
  for(unsigned shift : {0U,4U,8U}) {
    reset(); r[0x28/4] &= ~(15U<<shift); assert(plan().error);
    r[0x28/4] |= 6U<<shift; assert(plan().error); // Reserved width.
  }
  reset();
  for(auto range : {std::pair<uint64_t,uint64_t>{0x8cb5e9000ULL,0},
       {0xfffffffffULL,2}, {0xfffffffffffff000ULL,0x22000}, {0x1000000000ULL,1}})
    assert(planUfsSmmu(0x60,range.first,range.second,read).error);
  assert(!planUfsSmmu(0x60,0xfffffffffULL,1,read).error);
  r[0x28/4]&=~(1U<<12); assert(plan().error);
  reset(); r[0x20/4]&=~(1U<<30); assert(plan().error);
  // Reused context with an unexpected VMID/attribute must not be repaired.
  reset(); p=plan(); drop=0xffffffffU; assert(!configure());
  r[(0x1800+4*p.bank)/4]=0x10001; p=plan(); assert(p.error);
  writes.clear(); assert(configure() && writes.empty());
  // Hardware sync is asynchronous: no success until GSACTIVE clears; stuck hardware is bounded.
  for(unsigned busyReads : {0U,3U,100U}) {
    unsigned reads=0, ticks=0;
    uint32_t status=0xffffffffU;
    writes.clear();
    bool done=syncUfsSmmu([&](unsigned at) {
      assert(at==0x74 && writes.size()==2);
      assert(writes[0]==std::make_pair(0x68U,0xffffffffU));
      assert(writes[1]==std::make_pair(0x70U,0xffffffffU));
      return reads++ < busyReads ? 1U : 0U;
    }, [&](unsigned at,uint32_t value) {writes.emplace_back(at,value);},
    [&] {return ++ticks>=5;},status);
    assert(done==(busyReads<5) && status==(done?0U:1U));
    assert(reads==(done?busyReads+1:5U));
  }
  // Run-14 PKX110 IDR1: 128 global pages and 83 context banks need 0xd3000 bytes.
  reset(); r[0x24/4]=0x60000053;
  p=planUfsSmmu(0x60,0xbfdc3000,0x22000,read);
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
  assert(kUfsSmmuMapBytes==0x100000 && !p.error && p.pages==128 && p.bank==82);
  assert((p.pages+p.bank)*4096==0xd2000);
  writes.clear(); drop=0xffffffffU; assert(!configure());
  for(auto entry : writes) assert(entry.first+4<=kUfsSmmuMapBytes);
  assert(writes.front().first==0xd2000);
  p=planUfsSmmu(0x60,0xbfdc3000,0x22000,read); assert(!p.error && p.reuse);
  reset(); r[0x24/4]=0x60000053; r[0x800/4]=0x80000061; r[0xc00/4]=82;
  p=plan(); assert(!p.error && p.bank==81);
  reset(); r[0x24/4]=0x60000080; p=plan(); assert(!p.error && p.bank==127);
  reset(); r[0x24/4]=0x60000081; p=plan(); assert(p.error);
  // Run-15 register probe: 132 advertised groups, exact UFS entry on bank 0.
  // Other streams and bank-0 contents below are synthetic preservation checks.
  auto firmware=[&] {
    reset(); r[0x20/4]=0x4c017e84; r[0x24/4]=0x60000053;
    r[0x800/4]=0x80000060; r[0xc00/4]=0;
    r[0x804/4]=0x80000061; r[0xc04/4]=0;
    r[128*4096/4]=1; r[(128*4096+0x20)/4]=0x12345000;
    drop=0xffffffffU; writes.clear();
  };
  for(bool topOccupied : {false,true}) {
    firmware();
    if(topOccupied) {r[0x808/4]=0x80000062; r[0xc08/4]=82;}
    p=planUfsSmmu(0x60,0xbfdc3000,0x22000,read);
    assert(!p.error && !p.reuse && p.slot==0 && p.bank==(topOccupied?81U:82U));
    auto before=r;
    assert(!configure() && writes.size()==5);
    for(auto entry : writes) {
      unsigned at=entry.first;
      assert(at==(p.pages+p.bank)*4096 || at==0x1800+4*p.bank ||
             at==0x1000+4*p.bank || at==0xc00 || at==0x800);
      before.at(at/4)=r.at(at/4);
    }
    assert(before==r && read(0xc00)==p.bank);
  }
  firmware(); p=planUfsSmmu(0x60,0xbfdc3000,0x22000,read); drop=0xc00;
  assert(configure() && read(0xc00)==0 && read(0x800)==0x80000060);
  for(unsigned conflict : {0U,1U,2U}) {
    firmware();
    if(conflict==0) r[0x800/4]|=0x10000; // Masked UFS entry.
    if(conflict==1) r[0x804/4]=0x80000060; // Duplicate UFS entry.
    if(conflict==2) r[0xc00/4]=0x400000; // Unexpected privilege attribute.
    p=planUfsSmmu(0x60,0xbfdc3000,0x22000,read);
    assert(p.error && configure() && writes.empty());
  }
#else
  assert(p.error); // Preserve the older platform's mapped extent.
#endif
  writes.clear(); assert(configure() && writes.empty());
  // QUP SE DMA supplies its mapped extent explicitly and must leave the UFS bank intact.
  std::vector<uint32_t> qup(0x100000/4);
  auto qr=[&](unsigned at) {return qup.at(at/4);};
  auto qw=[&](unsigned at,uint32_t value) {qup.at(at/4)=value;};
  qup[0x20/4]=0x40000080;qup[0x24/4]=0x60000053;qup[0x28/4]=0x5111;
  qup[0x800/4]=0x80000060;qup[0xc00/4]=82;
  qup[(0x1000+4*82)/4]=0x10000;qup[(0x1800+4*82)/4]=1;
  assert(planUfsSmmu(0xa3,0x8cb5e9000ULL,8192,qr,0xd0000).error);
  auto q=planUfsSmmu(0xa3,0x8cb5e9000ULL,8192,qr,0x100000);
  assert(!q.error && q.bank==81 && q.slot==127);
  auto beforeQup=qup;
  assert(!configureUfsSmmu(q,0xa3,qr,qw));
  for(unsigned at : {(128+81)*4096U,0x1000+4*81U,0x1800+4*81U,0xc00+4*127U,0x800+4*127U})
    beforeQup.at(at/4)=qup.at(at/4);
  assert(beforeQup==qup && qr(0xc00)==82);
  q=planUfsSmmu(0xa3,0x8cb5e9000ULL,8192,qr,0x100000);assert(!q.error && q.reuse);
  for(unsigned conflict : {0U,1U,2U}) {
    qup[(0x800+4*127)/4]=0x800000a3;qup[(0xc00+4*127)/4]=81;qup[0x804/4]=0;
    if(conflict==0) qup[(0xc00+4*127)/4]=0; // Existing firmware bank 0 is not an approved QUP takeover.
    if(conflict==1) qup[(0x800+4*127)/4]|=0x10000;
    if(conflict==2) {qup[0x804/4]=0x800000a3;qup[0xc04/4]=81;}
    q=planUfsSmmu(0xa3,0x8cb5e9000ULL,8192,qr,0x100000);assert(q.error);
    assert(!__builtin_strcmp(q.error,"conflicting UFS stream mapping") && (q.smr&0xffff)==0xa3);
    if(conflict==0) assert(q.s2cr==0 && q.slot==127);
    beforeQup=qup;assert(configureUfsSmmu(q,0xa3,qr,qw) && qup==beforeQup);
  }
  puts("PASS: SMMU geometry/conflicts, VA64 high-DMA regression, full-range width limits, ordered configuration, reuse/repair and blocked-write refusal");
}

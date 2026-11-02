// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real production commands with a deterministic DMA/HC fault fixture.
#include "RootCommandsBridge.cpp"
#include <array>
#include <algorithm>
#include <string>
enum class Fault { None, Empty, Type, Tag, Lun, Query, Residual, Underflow,
  Oversize, SenseLength, CheckCondition, Timeout, StuckWithReply, Fatal, Stop, Start, HighBase, Aggregation,
  UicTimeout, UicResult, UicFatal, ExitTimeout, ExitDelayed, ExitStatus, StillHibern8, AutoHibernate, Preempted };
static Fault fault;
static std::array<uint8_t,kDmaBytes> device;
static std::array<uint32_t,256> registers;
static uint64_t now;
static unsigned writes, releases, sleeps;
static bool released;
static uint32_t txState;
static unsigned getCommands, exitCommands;
static uint64_t exitAt;

static uint64_t io(unsigned op,uint64_t address,uint64_t value,void *bytes)
{
  if (op==0) {
    if (address==0x20 && exitAt && now>=exitAt) { registers[0x20/4]|=0x20; exitAt=0; }
    return registers.at(address/4);
  }
  if (op==1) {
    ++writes;
    if (address==0x20) registers[address/4]&=~uint32_t(value);
    else if (!(address==0x60 && ((fault==Fault::Stop && !value) || (fault==Fault::Start && value))) &&
             !(address==0x54 && fault==Fault::HighBase) &&
             !(address==0x18 && fault==Fault::AutoHibernate) &&
             !(address==0x4c && fault==Fault::Aggregation)) registers.at(address/4)=uint32_t(value);
    if (address==0x90) {
      assert(registers[0x60/4]==0 && registers[0x58/4]==0 && releases==0);
      assert(!(registers[0x20/4]&0x400)); // Stale completion was acknowledged.
      if (value==1) {
        ++getCommands;
        assert(registers[0x94/4]==0x00410000);
        if (fault==Fault::UicTimeout) return 0;
        registers[0x9c/4]=txState;
        registers[0x98/4]=fault==Fault::UicResult?1:0;
        registers[0x20/4]|=fault==Fault::UicFatal?0x20400:0x400;
      } else {
        assert(value==0x18 && txState==1 && !(registers[0x20/4]&0x20));
        ++exitCommands;
        registers[0x20/4]|=(fault==Fault::ExitTimeout || fault==Fault::ExitDelayed)?0x400:0x420;
        if (fault==Fault::ExitDelayed) exitAt=now+250000;
        registers[0x30/4]=(fault==Fault::ExitStatus?0x20f:0x10f);
        if (fault!=Fault::StillHibern8) txState=5;
      }
    }
    if (address==0x58) {
      assert(released); released=false;
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
      assert(*reinterpret_cast<uint32_t *>(device.data()) & (1U<<24));
      assert(registers[0x24/4]==0); // Descriptor I bit does not enable CPU interrupts.
#endif
      if (fault==Fault::Fatal) { registers[0x20/4]=0x20000; return 0; }
      registers[0x58/4]=0;
      if (fault==Fault::Timeout) return 0;
      uint8_t *cmd=device.data()+kUcdOffset, *rsp=cmd+kRspOffset;
      std::fill(rsp,rsp+512,0);
      memset(device.data()+kDataOffset,0x7b,kDataMax);
      *reinterpret_cast<uint32_t *>(device.data()+8)=0;
      if (fault==Fault::Empty) return 0;
      memcpy(rsp,cmd,32); rsp[0]=cmd[0]+0x20; rsp[1]=0;
      if (fault==Fault::StuckWithReply || fault==Fault::Preempted) registers[0x58/4]=1;
      if (cmd[0]==1) putBe32(rsp+12,0);
      if (cmd[0]==0x16) putBe32(rsp+20,0x11);
      if (fault==Fault::Type) rsp[0]=0x3f;
      if (fault==Fault::Tag) ++rsp[3];
      if (fault==Fault::Lun) ++rsp[2];
      if (fault==Fault::Query) ++rsp[13];
      if (fault==Fault::Residual) putBe32(rsp+12,4096);
      if (fault==Fault::Underflow) rsp[1]=0x20;
      if (fault==Fault::Oversize) rsp[10]=0xff;
      if (fault==Fault::SenseLength) { rsp[11]=2; rsp[33]=18; }
      if (fault==Fault::CheckCondition) {
        rsp[7]=2; rsp[11]=20; rsp[33]=18; rsp[34]=0x70;
        rsp[36]=6; rsp[46]=0x29; rsp[47]=0;
      }
    }
  }
  if (op==2 || op==3) {
    assert(address>=gHost.phys && address-gHost.phys+value<=device.size());
    auto at=device.data()+(address-gHost.phys);
    if (op==2) { ++releases; memcpy(at,bytes,value); released=true; }
    else memcpy(bytes,at,value);
  }
  if (op==4) {
    if (fault==Fault::Preempted && registers[0x58/4]==1) {
      now+=1500000;
      registers[0x58/4]=0;
      registers[0x20/4]|=1;
    }
    return now++;
  }
  if (op==5) { ++sleeps; now+=value; }
  return 0;
}
static void reset(Fault f=Fault::None)
{
  fault=f; device.fill(0); registers.fill(0); now=0; writes=releases=sleeps=0; released=false;
  txState=5; getCommands=exitCommands=0; exitAt=0;
  registers[0x34/4]=1; registers[0x30/4]=15; registers[0x60/4]=1;
  ufs_test_reset(io,0x8cb5e9000);
}
static void assertQuarantined()
{
  assert(ufs_test_poisoned());
  std::array<uint8_t,kDmaBytes> old; memcpy(old.data(),memory,sizeof(memory));
  unsigned oldWrites=writes, oldReleases=releases;
  uint8_t cdb[6]={}; uint32_t scsi=0, value=0xabcdef;
  assert(ufs_test_nop()!=0);
  assert(ufs_test_query(3,2,&value)!=0 && value==0xabcdef);
  assert(ufs_test_scsi(0,cdb,6,false,0,&scsi)!=0);
  assert(writes==oldWrites && releases==oldReleases);
  assert(!memcmp(old.data(),memory,sizeof(memory)));
}
int main()
{
  reset(); assert(ufs_test_nop()==0);
  uint32_t value=0; assert(ufs_test_query(3,2,&value)==0 && value==0x11);
  std::array<uint8_t,131072> out;
  assert(ufs_test_read(0,1,32,out.data())==0);
  assert(std::all_of(out.begin(),out.end(),[](uint8_t x){return x==0x7b;}));
  assert(*reinterpret_cast<uint32_t *>(device.data()+20)==8);
  assert(*reinterpret_cast<uint32_t *>(device.data()+4096+1024+4)==8);
  assert(ufs_test_read(0,0xffffffffULL,1,out.data())==0); // Last READ(10) LBA is valid.
  auto oldWrites=writes;
  assert(ufs_test_read(0,0xffffffffULL,2,out.data())==kIOReturnBadArgument && writes==oldWrites);
  reset(Fault::Preempted); out.fill(0xa5);
  assert(ufs_test_read(0,1,1,out.data())==0);
  assert(!gHost.poisoned && gRootDiagnostic.command==1 && gRootDiagnostic.polls==2);
  assert(gRootDiagnostic.elapsedTicks>gRootDiagnostic.waitTicks);
  assert(gRootDiagnostic.ocs==0 && gRootDiagnostic.response==0x21000000);
  assert(std::all_of(out.begin(),out.begin()+4096,[](uint8_t x){return x==0x7b;}));
  for (Fault f: {Fault::Empty,Fault::Type,Fault::Tag,Fault::Lun,Fault::Residual,
                Fault::Underflow,Fault::Oversize,Fault::SenseLength,Fault::Timeout,Fault::Fatal}) {
    reset(f); out.fill(0xa5);
    assert(ufs_test_read(0,1,1,out.data())!=0);
    assert(std::all_of(out.begin(),out.end(),[](uint8_t x){return x==0xa5;}));
    assertQuarantined();
  }
  reset(Fault::Query); value=0xabcdef;
  assert(ufs_test_query(3,2,&value)!=0 && value==0xabcdef); assertQuarantined();
  reset(Fault::CheckCondition); out.fill(0xa5);
  assert(ufs_test_read(0,1,1,out.data())==kIOReturnNotReady);
  assert(!gHost.poisoned && gRootDiagnostic.sense==0x062900);
  assert(out[0]==0xa5);
  fault=Fault::None; assert(ufs_test_read(0,1,1,out.data())==0);
  reset(); uint8_t cdb[17]={}; uint32_t scsi;
  assert(ufs_test_scsi(0,cdb,17,false,0,&scsi)==kIOReturnBadArgument);
  assert(ufs_test_scsi(0,cdb,6,true,kDataMax+4,&scsi)==kIOReturnBadArgument);
  assert(ufs_test_scsi(0,cdb,6,false,4096,&scsi)==kIOReturnBadArgument); // No writes to media.
  assert(writes==0 && releases==0);
  for(Fault f : {Fault::Timeout,Fault::StuckWithReply}) {
    reset(f); assert(ufs_test_nop()==kIOReturnTimeout);
    assert(gRootDiagnostic.waitTicks>=200000 && gRootDiagnostic.waitTicks<=200010);
    assert(gRootDiagnostic.elapsedTicks>=200000 && gRootDiagnostic.elapsedTicks<=200010);
    assert(gRootDiagnostic.polls>2);
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
    assert(sleeps==0);
#endif
    assert(gRootDiagnostic.dmaOcs==(f==Fault::Timeout?15U:0U));
    assert(gRootDiagnostic.dmaResponse==(f==Fault::Timeout?0U:0x20000001U));
    assertQuarantined(); // A DMA reply while DB=1 must remain a timeout.
  }
  reset();
#if defined(NATIVE_UFS_READ_ONLY) && NATIVE_UFS_READ_ONLY
  assert(!ufs_test_store_open(100, 4) && !ufs_test_log_open(200, 4));
  cdb[0]=0x2a;
  assert(hostScsiDir(0,cdb,10,1,4096,200000,&scsi)==kIOReturnBadArgument);
  cdb[0]=0x42; // UNMAP is also prohibited, including a malformed no-data request.
  assert(hostScsiDir(0,cdb,10,0,0,200000,&scsi)==kIOReturnBadArgument);
  assert(writes==0 && releases==0);
#endif
  reset(); registers[0]=0x40000000; registers[0x300/4]=1;
  assert(!ufs_test_stop_queue() && writes==0);
  reset(); registers[0]=0x40000000; registers[0x300/4]=2; // ESI alone is not MCQ mode.
  assert(ufs_test_stop_queue());
  reset(); registers[0]=0x40000000; registers[0x300/4]=0;
  assert(ufs_test_stop_queue());
  for (unsigned offset: {0x58U,0x78U}) {
    reset(); registers[offset/4]=1;
    assert(!ufs_test_stop_queue() && writes==0);
  }
  reset(); registers[0x34/4]=0; assert(!ufs_test_stop_queue() && writes==0);
  reset(); registers[0x20/4]=0x20000;
  assert(!ufs_test_stop_queue() && writes==0);
  assert(ufs_test_nop()!=0 && writes==0 && releases==0); assertQuarantined();
  reset(Fault::Stop); assert(!ufs_test_stop_queue() && now>=100000);
  reset(Fault::Start); assert(ufs_test_stop_queue()); assert(!ufs_test_start_queue());
  reset(Fault::HighBase); assert(ufs_test_stop_queue()); assert(!ufs_test_start_queue());
  reset(); assert(ufs_test_stop_queue() && ufs_test_start_queue());
  assert(registers[0x50/4]==0xcb5e9000 && registers[0x54/4]==8);
#if defined(NATIVE_UFS_SM8750) && NATIVE_UFS_SM8750
  assert(getCommands==1 && exitCommands==0 && gRootDiagnostic.txBefore==5);
  reset(); txState=1; registers[0x20/4]=0x420; registers[0x18/4]=0x1501;
  assert(ufs_test_stop_queue() && ufs_test_start_queue());
  assert(getCommands==2 && exitCommands==1 && sleeps==0);
  assert(gRootDiagnostic.txBefore==1 && gRootDiagnostic.txAfter==5);
  assert(gRootDiagnostic.linkAhit==0x1501 && registers[0x18/4]==0);
  assert(ufs_test_nop()==0);
  reset(Fault::ExitDelayed); txState=1;
  assert(ufs_test_stop_queue() && ufs_test_start_queue() && now>=250000 && sleeps==0);
  assert(exitCommands==1 && gRootDiagnostic.txAfter==5);
  for (Fault f: {Fault::UicTimeout,Fault::UicResult,Fault::UicFatal,Fault::ExitTimeout,
                Fault::ExitStatus,Fault::StillHibern8,Fault::AutoHibernate}) {
    reset(f); txState=1; registers[0x20/4]=0x420; registers[0x18/4]=0x1501;
    assert(ufs_test_stop_queue() && !ufs_test_start_queue());
    assert(registers[0x50/4]==0 && registers[0x60/4]==0 && releases==0 && sleeps==0);
    assert(gRootDiagnostic.result!=0);
    if (f==Fault::UicTimeout || f==Fault::ExitTimeout) assert(now>=100000);
    if (f==Fault::ExitTimeout) assert(now>=500000 && now<500100);
    if (f==Fault::UicFatal) assert(registers[0x20/4]&0x20000);
  }
  reset(); assert(ufs_test_stop_queue()); registers[0x30/4]&=~8U;
  assert(!ufs_test_start_queue() && getCommands==0 && exitCommands==0 && releases==0);
  reset(); registers[0x4c/4]=0x81001f20;
  assert(ufs_test_stop_queue() && ufs_test_start_queue());
  assert(gRootDiagnostic.iacrBefore==0x81001f20 && gRootDiagnostic.iacr==0);
  assert(registers[0x4c/4]==0 && ufs_test_nop()==0);
  reset(Fault::Aggregation); registers[0x4c/4]=0x81001f20;
  assert(ufs_test_stop_queue() && !ufs_test_start_queue());
  assert(registers[0x50/4]==0 && registers[0x60/4]==0 && releases==0);
#endif
  puts("PASS: actual command/queue path; malformed DMA responses, bounds, short reads, sense, quarantine, queue refusal and full-width DMA");
}

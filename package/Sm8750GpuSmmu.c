/* SPDX-License-Identifier: MIT */
#include "Sm8750GpuSmmu.h"
static int rd(const ArmSmmu4KIo *io,size_t at,uint32_t *v,Sm8750GpuSmmuResult *r)
{
    r->offset=(uint32_t)at;r->value=UINT32_MAX;
    if(io->read32(io->cookie,at,&r->value))return -5;
    *v=r->value;return 0;
}
static int live(uint32_t id0,uint32_t smr,uint32_t s2cr)
{return id0&0x100?!!(s2cr&0x400):!!(smr&0x80000000u);}
int Sm8750GpuSmmuPrepare(const ArmSmmu4KIo *io,Sm8750GpuSmmuResult *r)
{
    if(!r)return -22;
    *r=(Sm8750GpuSmmuResult){0};
    if(!io || !io->read32 || !io->write32 || !io->barrier || io->register_bytes<0xc04)return -22;
    r->phase=1;
    uint32_t cr0,fault;
    if(rd(io,0x20,&r->id0,r) || rd(io,0x24,&r->id1,r) || rd(io,0,&cr0,r) || rd(io,0x48,&fault,r))return -5;
    unsigned slots=r->id0&255,banks=r->id1&255,s2=(r->id1>>16)&255;
    size_t page=(r->id1>>31)?0x10000:0x1000,pages=(size_t)1<<(((r->id1>>28)&7)+1);
    unsigned extended=!!(r->id0&0x100);
    uint32_t sidmask=extended?0xffffu:(1u<<((r->id0>>9)&15))-1;
    if((r->id0&0x48000000)!=0x48000000 || slots<3 || slots>128 || !(sidmask&4) ||
        banks<3 || banks>pages || s2 || pages*page*2>io->register_bytes ||
        0xc00+slots*4>io->register_bytes)return -95;
    if((cr0&0x407)!=0x406 || !!(cr0&8)!=extended || fault)return -5;
    r->phase=2;
    uint64_t used_banks=0;
    unsigned found=0;
    const unsigned sids[]={0,1,5};
    for(unsigned i=0;i<slots;++i) {
        uint32_t smr,s2cr;
        if(rd(io,0x800+4*i,&smr,r) || rd(io,0xc00+4*i,&s2cr,r))return -5;
        if(live(r->id0,smr,s2cr)) {
            uint32_t mask=(smr>>16)&(extended?0xffffu:0x7fffu);
            for(unsigned j=0;j<3;++j)if(!((sids[j]^smr)&~mask&sidmask))return -16;
            if(!(s2cr&0x30000) && (s2cr&255)<64)used_banks|=UINT64_C(1)<<(s2cr&255);
        } else if(found<3)r->slots[found++]=i;
    }
    if(found<3)return -28;
    r->phase=3;r->banks[0]=0;
    /* OnePlus qcom_adreno_smmu_alloc_context_bank reserves 0/1 for GPU; GMU starts at 2. */
    uint32_t sctlr;
    if(rd(io,pages*page,&sctlr,r))return -5;
    if((sctlr&1) || (used_banks&1))return -16;
    unsigned bank;
    for(bank=2;bank<banks && bank<64;++bank) {
        if(used_banks&(UINT64_C(1)<<bank))continue;
        if(rd(io,(pages+bank)*page,&sctlr,r))return -5;
        if(!(sctlr&1))break;
    }
    if(bank>=banks || bank>=64)return -28;
    r->banks[1]=bank;
    for(unsigned i=0;i<2;++i)r->bank_offset[i]=(pages+r->banks[i])*page;
    /* Recheck every chosen slot and bank before the first write. */
    r->phase=4;
    for(unsigned i=0;i<3;++i) {
        uint32_t smr,s2cr;
        if(rd(io,0x800+4*r->slots[i],&smr,r) || rd(io,0xc00+4*r->slots[i],&s2cr,r))return -5;
        if(live(r->id0,smr,s2cr))return -16;
    }
    for(unsigned i=0;i<2;++i) {
        if(rd(io,r->bank_offset[i],&sctlr,r))return -5;
        if(sctlr&1)return -16;
    }
    r->phase=5;
    for(unsigned i=0;i<3;++i) {
        uint32_t value;size_t at=0xc00+4*r->slots[i];
        r->offset=(uint32_t)at;r->value=UINT32_MAX;
        if(io->write32(io->cookie,at,0x20000) || io->barrier(io->cookie) || rd(io,at,&value,r))return -5;
        if(value!=0x20000)return -5;
    }
    r->phase=6;return 0;
}

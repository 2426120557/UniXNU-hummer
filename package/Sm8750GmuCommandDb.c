/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Derived from UniXNU-sm8550-common 23b2049fcd9e30356161def84231334fb55ecca9;
 * SM8750 reservation and optional BX rail come from the device DT and Gen8 source. */
#include "Sm8750GmuCommandDb.h"
static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p){return u16(p)|(uint32_t)u16(p+2)<<16;}
static int range(size_t at,size_t len,size_t size){return at<=size && len<=size-at;}
static int overlaps(size_t a,size_t n,size_t b,size_t m){return n && m && a<b+m && b<a+n;}
static int equal(const uint8_t *a,const char *b)
{for(unsigned i=0;i<8;++i)if(a[i]!=(uint8_t)b[i])return 0;return 1;}
Sm8750GmuDbStatus Sm8750GmuCommandDbInputs(const uint8_t *data,size_t bytes,Sm8750GmuVoteInputs *out)
{
    if(!data || !out || bytes!=0x20000)return Sm8750GmuDbArgument;
    if(u32(data)!=1)return Sm8750GmuDbVersion;
    if(u32(data+4)!=0x0c0330db)return Sm8750GmuDbCorrupt;
    size_t heads[8]={0},sizes[8]={0},aux[8]={0};unsigned rows=0,total=0;
    for(unsigned i=0;i<8;++i) {
        const uint8_t *r=data+8+16*i;unsigned id=u16(r),count=u16(r+6);
        if(!id)break;
        heads[i]=144u+u16(r+2);sizes[i]=(size_t)count*24;aux[i]=144u+u16(r+4);
        total+=count;
        if(total>4096 || !range(heads[i],sizes[i],bytes) || !range(aux[i],0,bytes))return Sm8750GmuDbCorrupt;
        for(unsigned j=0;j<i;++j)if(overlaps(heads[i],sizes[i],heads[j],sizes[j]))return Sm8750GmuDbCorrupt;
        ++rows;
    }
    static const char names[9][8]={{'g','f','x','.','l','v','l',0},{'m','x','.','l','v','l',0,0},
        {'c','x','.','l','v','l',0,0},{'g','m','x','c','.','l','v','l'},
        {'S','H','0',0,0,0,0,0},{'M','C','0',0,0,0,0,0},{'A','C','V',0,0,0,0,0},
        {'C','N','0',0,0,0,0,0},{'g','b','x','.','l','v','l',0}};
    AdrenoHfiRail found[9]={{0}};uint32_t addresses[9]={0};unsigned seen=0;
    for(unsigned i=0;i<rows;++i)for(size_t at=heads[i];at<heads[i]+sizes[i];at+=24) {
        const uint8_t *e=data+at;uint32_t address=u32(e+16);size_t len=u16(e+20),offset=aux[i]+u16(e+22);
        unsigned slave=u16(data+8+16*i);
        if(!range(offset,len,bytes))return Sm8750GmuDbCorrupt;
        for(unsigned name=0;name<9;++name)if(equal(e,names[name])) {
            if(seen&(1u<<name))return Sm8750GmuDbDuplicate;
            if(!address || (address&3) || ((address>>16)&7)!=slave)return Sm8750GmuDbCorrupt;
            for(unsigned j=0;j<rows;++j)if(overlaps(offset,len,heads[j],sizes[j]))return Sm8750GmuDbCorrupt;
            if(slave!=((name<4 || name==8)?3u:5u))return Sm8750GmuDbCorrupt;
            if(name!=7 && !len)return Sm8750GmuDbCorrupt;
            seen|=1u<<name;found[name]=(AdrenoHfiRail){data+offset,len};addresses[name]=address;
        }
    }
    if((seen&0xf7)!=0xf7)return Sm8750GmuDbMissing;
    Sm8750GmuVoteInputs result={0};
    result.gfx=found[0];result.mx=found[1];result.cx=found[2];result.gmxc=found[3];result.gbx=found[8];
    result.sh0=(Sm8750GmuBcmInput){addresses[4],found[4]};result.mc0=(Sm8750GmuBcmInput){addresses[5],found[5]};
    result.acv=(Sm8750GmuBcmInput){addresses[6],found[6]};result.cn0_address=addresses[7];
    result.cx_address=addresses[2];result.mx_address=addresses[1];
    *out=result;return Sm8750GmuDbSuccess;
}

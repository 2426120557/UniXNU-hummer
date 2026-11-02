/* SPDX-License-Identifier: MIT */
/* Layout facts: Qualcomm smem.c, socinfo.h and adreno_read_soc_code. */
#include "Sm8750SocInfo.h"
static uint32_t le16(const volatile uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const volatile uint8_t *p){return le16(p)|le16(p+2)<<16;}
static int item(const volatile uint8_t *p,size_t bytes,uint32_t *id,uint32_t *code)
{
    if(bytes<184 || le32(p)<16 || le32(p)>22)return -71;
    uint32_t fc=le32(p+176),pc=le32(p+180);
    if(fc>=0x101)fc=0;
    if(fc<0xf1 || fc>=0x101 || pc>=0x7fffffff)pc=0;
    if(pc>0xffff)return -71;
    *id=le32(p+4);*code=(pc<<16)|fc;return 0;
}
int Sm8750SocInfoGpuCode(const volatile uint8_t *data,size_t bytes,uint32_t *soc_id,uint32_t *soc_code)
{
    if(!data || bytes!=0x200000 || !soc_id || !soc_code || soc_id==soc_code)return -22;
    if(le32(data+192)!=1 || le32(data+204) || (le32(data+92)>>16)!=12)return -71;
    const volatile uint8_t *toc=data+bytes-4096;
    if(le32(toc)!=0x434f5424 || le32(toc+4)!=1)return -71;
    unsigned count=le32(toc+8);if(count>84)return -71;
    size_t start=0,size=0,line=0;
    for(unsigned i=0;i<count;++i) {
        const volatile uint8_t *e=toc+32+i*48;
        if(le16(e+12)!=0xfffe || le16(e+14)!=0xfffe || !le32(e) || !le32(e+4))continue;
        if(start)return -71;
        start=le32(e);size=le32(e+4);line=le32(e+16);
        if((start&7) || size<32 || start>bytes-4096 || size>bytes-4096-start ||
            line<16 || line>4096 || (line&(line-1)))return -71;
    }
    if(!start)return -2;
    const volatile uint8_t *part=data+start;
    if(le32(part)!=0x54525024 || le32(part+4)!=0xfffefffe || le32(part+8)!=size)return -71;
    size_t uncached=le32(part+12),cached=le32(part+16);
    if(uncached<32 || uncached>cached || cached>size)return -71;
    const volatile uint8_t *found=0;size_t found_size=0;
    for(size_t at=32;at<uncached;) {
        if(uncached-at<16)return -71;
        const volatile uint8_t *e=part+at;size_t n=le32(e+4),pad=le16(e+8),hdr=le16(e+10);
        if(le16(e)!=0xa5a5 || pad>n || hdr>uncached-at-16 || n>uncached-at-16-hdr)return -71;
        if(le16(e+2)==137) {if(found)return -71;found=e+16+hdr;found_size=n-pad;}
        at+=16+hdr+n;
    }
    for(size_t end=size;end>cached;) {
        if(end-cached<line)return -71;
        const volatile uint8_t *e=part+end-line;size_t n=le32(e+4),pad=le16(e+8);
        if(le16(e)!=0xa5a5 || pad>n || n>end-line-cached)return -71;
        if(le16(e+2)==137) {if(found)return -71;found=e-n;found_size=n-pad;}
        end-=line+n;
    }
    if(!found)return -2;
    uint32_t id,code;int rc=item(found,found_size,&id,&code);
    if(!rc){*soc_id=id;*soc_code=code;}return rc;
}

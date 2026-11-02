/* SPDX-License-Identifier: MIT */
#include "Sm8750GpuZap.h"
int Sm8750GpuSecureCxInit(const Sm8750ScmIo *io,Sm8750ScmResult *out)
{return Sm8750ScmCall(io,0x42002801,1,9,0,0,out);}
int Sm8750ScmCall(const Sm8750ScmIo *io,uint32_t function,uint32_t arginfo,
    uint64_t a,uint64_t b,uint64_t c,Sm8750ScmResult *out)
{
    if(!io || !io->invoke || !io->now_us || !io->wait_us || !out)return -22;
    *out=(Sm8750ScmResult){0};uint64_t start,now,fn=function,a6=0;
    if(io->now_us(io->cookie,&start))return -5;
    for(unsigned retries=0;out->calls<10000;) {
        if(io->now_us(io->cookie,&now) || now<start)return -5;
        if(now-start>=1000000)return -110;
        uint64_t r[8]={fn,arginfo,a,b,c,0,a6,0};++out->calls;
        if(io->invoke(io->cookie,r))return -5;
        out->transport=(int64_t)r[0];out->result=r[1];
        if(r[0]==1) {fn=1;a6=r[6];continue;}
        if((int64_t)r[0]==-12 && retries++<20) {
            fn=function;a6=0;if(io->wait_us(io->cookie,30000))return -5;
            continue;
        }
        return r[0] || r[1]?-5:0;
    }
    return -110;
}
#if defined(__aarch64__)
int Sm8750ScmInvoke(void *cookie,uint64_t r[8])
{
    (void)cookie;
    register uint64_t x0 __asm__("x0")=r[0],x1 __asm__("x1")=r[1],x2 __asm__("x2")=r[2],x3 __asm__("x3")=r[3];
    register uint64_t x4 __asm__("x4")=r[4],x5 __asm__("x5")=r[5],x6 __asm__("x6")=r[6],x7 __asm__("x7")=r[7];
    __asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)
        :: "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
    r[0]=x0;r[1]=x1;r[2]=x2;r[3]=x3;r[4]=x4;r[5]=x5;r[6]=x6;r[7]=x7;return 0;
}
#endif
int Sm8750GpuZapLoad(const Sm8750ScmIo *io,const uint8_t *metadata,size_t bytes,
    const uint8_t *payload,size_t payload_bytes,void *cpu,uint64_t physical,
    volatile uint8_t *reservation,Sm8750GpuZapResult *out)
{
    if(!out)return -22;
    *out=(Sm8750GpuZapResult){0};
    if(!io || !io->publish || !io->invoke || !io->now_us || !io->wait_us ||
        !metadata || !payload || !cpu || !reservation || bytes!=0xfcc || payload_bytes!=0x1000 ||
        !physical || (physical&0xfff) || physical>UINT64_C(0x10000000000)-0x4000 ||
        (physical<UINT64_C(0x9b09c000) && physical+0x4000>UINT64_C(0x9b09a000)) ||
        ((uintptr_t)cpu&0xfff) || ((uintptr_t)reservation&0xfff))return -22;
    out->phase=1;
    for(size_t i=0;i<0x4000;++i)((uint8_t *)cpu)[i]=i<bytes?metadata[i]:0;
    if(io->publish(io->cookie,cpu,0x4000))return -5;
    out->phase=2;
    int status=Sm8750ScmCall(io,0x42000201,0x82,13,physical,0,&out->scm);
    if(status)return status;
    out->phase=3;
    status=Sm8750ScmCall(io,0x42000202,3,13,0x9b09a000,payload_bytes,&out->scm);
    if(status)return status;
    out->phase=4;
    for(size_t i=0;i<payload_bytes;++i)reservation[i]=payload[i];
    if(io->publish(io->cookie,(const void *)reservation,payload_bytes))return -5;
    out->phase=5;
    status=Sm8750ScmCall(io,0x42000205,1,13,0,0,&out->scm);
    if(status)return status;
    out->authenticated=1;out->phase=6;return 0;
}

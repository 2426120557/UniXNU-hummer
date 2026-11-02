/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Register facts: OnePlusOSS/android_kernel_oneplus_sm8750
 * 745702d5ed2c0435f2b1d3bb78bc4f48422d4191 gpucc-sun.c/gcc-sun.c,
 * clk-alpha-pll.h (Taycan ELU uses Lucid EVO) and Qualcomm gdsc.c. */
#include "Sm8750GpuPower.h"
typedef struct {const Sm8750GpuPowerIo *io;Sm8750GpuPowerResult *r;} Power;
static int rd(Power *p,unsigned w,uint32_t at,uint32_t *v)
{
    p->r->window=w;p->r->offset=at;++p->r->reads;
    if(p->io->read(p->io->cookie,w,at,v))return -5;
    p->r->value=*v;return *v==UINT32_MAX?-5:0;
}
static int wr(Power *p,unsigned w,uint32_t at,uint32_t v)
{
    p->r->window=w;p->r->offset=at;p->r->value=v;
    ++p->r->writes;p->r->retain_resources=1;
    return p->io->write(p->io->cookie,w,at,v)?-5:0;
}
static int wait_us(Power *p,unsigned us){return p->io->delay(p->io->cookie,us)?-5:0;}
static int poll(Power *p,unsigned w,uint32_t at,uint32_t mask,uint32_t value,unsigned budget)
{
    for(unsigned i=0;i<budget;++i) {
        uint32_t v;int e=rd(p,w,at,&v);if(e)return e;
        if((v&mask)==value)return 0;
        if(wait_us(p,1))return -5;
    }
    return -110;
}
static int update(Power *p,unsigned w,uint32_t at,uint32_t mask,uint32_t value)
{
    uint32_t v;int e=rd(p,w,at,&v);if(e)return e;
    if((v&mask)==value)return 0;
    e=wr(p,w,at,(v&~mask)|value);if(e)return e;
    return poll(p,w,at,mask,value,100);
}
static int branch(Power *p,unsigned w,uint32_t at,unsigned mode)
{
    uint32_t value;int e=rd(p,w,at,&value);if(e)return e;
    if(!(value&1) && (e=wr(p,w,at,value|1)))return e;
    if(wait_us(p,10))return -5;
    if(!mode)return 0;
    /* branch2 acknowledges CLK_OFF clear or NoC FSM ON, not the enable bit. */
    for(unsigned i=0;i<500;++i) {
        if((e=rd(p,w,at,&value)))return e;
        if(!(value&(1u<<31)) || (value&0x70000000)==0x20000000)return 0;
        if(wait_us(p,1))return -5;
    }
    return -110;
}
static int rcg(Power *p,uint32_t at,uint32_t cfg)
{
    int e=update(p,1,at,2,2);if(e)return e;
    if((e=poll(p,1,at,1u<<31,0,500)))return e;
    if((e=update(p,1,at+4,0x0010371f,cfg)))return e;
    uint32_t command;
    if((e=rd(p,1,at,&command)))return e;
    if((e=wr(p,1,at,command|1)))return e;
    if((e=poll(p,1,at,1,0,500)))return e;
    return update(p,1,at,2,0);
}
static int pll(Power *p)
{
    uint32_t mode,op,l,alpha,user;int e;
    if(rd(p,1,0,&mode) || rd(p,1,4,&op) || rd(p,1,0x10,&l) ||
       rd(p,1,0x14,&alpha) || rd(p,1,0x18,&user))return -5;
    /* Never disable a bootloader clock to change an unknown active rate. */
    if((mode&1) || op) {
        if(op!=1 || (mode&0x80000005)!=0x80000005 || (l&0xffff)!=0x34 ||
           (alpha&0xffff)!=0x1555 || (user&0x3c07)!=0x407 || (user&(1u<<25)))return -16;
        return 0;
    }
    if(user&(1u<<25))return -16;
    static const uint32_t config[][2]={{0x10,0x00440034},{0x14,0x1555},
        {0x20,0x19660387},{0x24,0x098060a0},{0x28,0xb416cb20},{0x18,0x400},{0x1c,2}};
    for(unsigned i=0;i<sizeof(config)/sizeof(*config);++i)
        if((e=update(p,1,config[i][0],UINT32_MAX,config[i][1])))return e;
    if((e=update(p,1,0,4,4)))return e;
    if((e=update(p,1,4,UINT32_MAX,1)))return e;
    if((e=poll(p,1,0,1u<<31,1u<<31,1500)))return e;
    if((e=update(p,1,0x18,7,7)))return e;
    return update(p,1,0,1,1);
}
int Sm8750GpuPowerOn(const Sm8750GpuPowerIo *io,Sm8750GpuPowerResult *r)
{
    if(!io || !r || !io->read || !io->write || !io->delay || !io->rails)return -22;
    if(r->attempted)return -16;
    *r=(Sm8750GpuPowerResult){0};r->attempted=1;
    Power p={io,r};int e;uint32_t reset;
    r->stage=1;
    /* Both clocks require LOW_SVS (64). The rail owner preserves higher users. */
    r->retain_resources=1;
    if((e=io->rails(io->cookie,64,64)))return e;
    r->stage=2;
    if((e=branch(&p,0,0x71004,0)))return e;
    if(rd(&p,1,0x907c,&reset))return -5;
    if(reset&1)return -16;
    if(rd(&p,1,0x9314,&reset))return -5;
    if(reset&1)return -16;
    r->stage=3;
    /* GMU-CX and SMMU-CX are references to this same parent GDSC. */
    if((e=update(&p,1,0x9080,0x00fff007,0x00228000)))return e;
    if(wait_us(&p,1))return -5;
    if((e=poll(&p,1,0x9094,1u<<31,1u<<31,2000)))return e;
    if(wait_us(&p,1))return -5;
    if((e=update(&p,1,0x9080,1u<<11,1u<<11)))return e;
    r->stage=4;
    if((e=update(&p,0,0x52000,3u<<15,3u<<15)))return e;
    if(wait_us(&p,10))return -5;
    /* GPLL0 belongs to GCC. Confirm its inherited 600 MHz rate, never retune it. */
    uint32_t l,alpha;
    if(rd(&p,0,0,&reset) || rd(&p,0,0x10,&l) || rd(&p,0,0x14,&alpha))return -5;
    if(!(reset&(1u<<31)) || (l&0xffff)!=31 || (alpha&0xffff)!=0x4000)return -95;
    if((e=pll(&p)))return e;
    r->stage=5;
    if((e=rcg(&p,0x93ec,0x505)))return e;
    if((e=rcg(&p,0x9318,0x201)))return e;
    r->stage=6;
    static const uint32_t critical[]={0x93a4,0x9008,0x9064,0x93a8,0x9004,0x90cc};
    for(unsigned i=0;i<sizeof(critical)/sizeof(*critical);++i)
        if((e=branch(&p,1,critical[i],0)))return e;
    static const uint32_t clocks[]={0x9010,0x90d4,0x90e4,0x93e8,0x90e8,0x7000};
    if((e=branch(&p,1,0x90bc,0)))return e;
    for(unsigned i=0;i<sizeof(clocks)/sizeof(*clocks);++i)
        if((e=branch(&p,1,clocks[i],1)))return e;
    if((e=branch(&p,0,0x71150,0)))return e;
    if((e=branch(&p,0,0x71010,1)))return e;
    r->stage=7;r->ready=1;return 0;
}
int Sm8750GpuRailCommands(const Sm8750GmuVoteInputs *in,uint32_t cx,uint32_t mx,struct rpmh_command out[2])
{
    if(!in || !out || cx<64 || mx<64 || (in->cx_address>>16)!=3 || (in->mx_address>>16)!=3 ||
       ((in->cx_address|in->mx_address)&3) || in->cx_address==in->mx_address)return -22;
    const uint32_t c[]={0,cx},m[]={0,mx};uint32_t cv[2],mv[2];
    if(AdrenoHfiArcVotes(c,2,in->cx.data,in->cx.bytes,in->cx.data,in->cx.bytes,cv,2) ||
       AdrenoHfiArcVotes(m,2,in->mx.data,in->mx.bytes,in->mx.data,in->mx.bytes,mv,2))return -22;
    out[0]=(struct rpmh_command){in->mx_address,mv[1]&255};
    out[1]=(struct rpmh_command){in->cx_address,cv[1]&255};return 0;
}

// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>
#include "Buttons.h"
#include "S3910.h"
#include "Reports.h"
#include <GeniSpi.h>
#include <PeripheralIO.h>
#include <Rpmh.h>
#include <PaganiPeripherals.h>
#include "../../BootProgress/PeripheralStatus.h"

static unsigned spiReads,spiWrites,spiSnapshots,dmaSnapshots,readSnapshots,statusReads,timingReads;
static int spiResult;
static uint8_t spiRxByte=0xa5;
static uint32_t spiSnapshot[6];
static uint32_t dmaSnapshot[4];
static uint32_t readSnapshot[8];
struct PeripheralWindow {
    uint32_t values[2]={1,0};unsigned reads=0;
    static uint32_t read(void *ctx,uint32_t reg) {
        assert(reg==4);auto &p=*static_cast<PeripheralWindow *>(ctx);
        return p.values[p.reads++?1:0];
    }
};
static uint32_t spiStatus(void *,uint32_t reg) {
    if (reg==SM8750_SPI_DELAY_COUNTERS) {++timingReads;return 0xc800;}
    assert(reg==SE_GENI_STATUS);++statusReads;return 0x40;
}
void boot_progress_touch_read(uint32_t length,uint32_t head,uint32_t pins,uint32_t dma,
                              uint32_t state,uint32_t txin,uint32_t rxin,uint32_t delay) {
    const uint32_t values[]={length,head,pins,dma,state,txin,rxin,delay};
    memcpy(readSnapshot,values,sizeof values);++readSnapshots;
}
void boot_progress_touch_dma(uint32_t tx,uint32_t rx,uint32_t txin,uint32_t rxin) {
    const uint32_t values[]={tx,rx,txin,rxin};memcpy(dmaSnapshot,values,sizeof values);++dmaSnapshots;
}
void boot_progress_touch_spi(uint32_t fw,uint32_t disabled,uint32_t state,uint32_t irq,uint32_t tx,uint32_t rx) {
    const uint32_t values[]={fw,disabled,state,irq,tx,rx};
    memcpy(spiSnapshot,values,sizeof values);++spiSnapshots;
}
static int touchTransfer(geni_spi *spi,const uint8_t *tx,uint8_t *rx,size_t size,int keep) {
    assert(tx && size && !keep && !spi->snap_valid);
    if (rx) {
        for (size_t i=0;i<size;++i) assert(tx[i]==0xff);
        memset(rx,spiRxByte,size);++spiReads;
    } else ++spiWrites;
    if (spiResult) {
        spi->snap_valid=1;spi->snap_status=1;spi->snap_irq=0x40000000;
        spi->snap_txfs=1;spi->snap_rxfs=0;
        spi->snap_dma_tx=4;spi->snap_dma_rx=0;spi->snap_dma_txin=2;spi->snap_dma_rxin=1;
    }
    spi->last_tx_in=spi->use_dma?(uint32_t)size:0;
    spi->last_rx_in=spi->use_dma && rx?(uint32_t)size:0;
    return spiResult;
}
struct PaganiInput {
    geni_spi spi={};
    bool dmaExposed=false,firstReadCaptured=false;
    PeripheralWindow pins[6];
    PaganiInput() {spi.se.rd=spiStatus;pins[4].values[0]=pins[4].values[1]=3;}
#define geni_spi_transfer touchTransfer
#include "PaganiReadTouch.inc"
#undef geni_spi_transfer
};
static void spiRead() {
    PaganiInput device;uint8_t bytes[S3910_PAYLOAD_MAX+8]={};
    device.spi.info.fw_revision=0x101;device.spi.info.if_disable=1;
    assert(!PaganiInput::readTouch(&device,bytes,4) && bytes[0]==0xa5 && spiReads==1);
    const uint32_t expectedRead[]={4,0xa5a5a5a5,13,0,0x40,0,0,0xc800};
    assert(readSnapshots==1 && statusReads==1 && timingReads==1 && !memcmp(readSnapshot,expectedRead,sizeof expectedRead));
    assert(!PaganiInput::readTouch(&device,bytes,sizeof(bytes)-1) && spiReads==2);
    assert(readSnapshots==1 && statusReads==1 && timingReads==1 && device.pins[5].reads==2);
    assert(!spiSnapshots);
    spiResult=-110;assert(PaganiInput::readTouch(&device,bytes,4)==-110 && spiReads==3);
    const uint32_t expected[]={0x101,1,1,0x40000000,1,0};
    assert(spiSnapshots==1 && !memcmp(spiSnapshot,expected,sizeof expected));
    assert(PaganiInput::readTouch(&device,bytes,sizeof bytes)==-90 && spiReads==3);
    assert(PaganiInput::readTouch(&device,nullptr,4)==-22 && spiReads==3);
    assert(PaganiInput::readTouch(&device,bytes,0)==-22 && spiReads==3);
    assert(spiSnapshots==1);
    assert(PaganiInput::writeTouch(&device,bytes,4)==-110 && spiWrites==1 && spiSnapshots==2);
    spiResult=0;assert(!PaganiInput::readTouch(&device,bytes,4) && !device.spi.snap_valid && spiSnapshots==2);
    assert(!device.dmaExposed && !dmaSnapshots);
    device.spi.use_dma=1;
    assert(!PaganiInput::readTouch(&device,bytes,4) && device.dmaExposed && !dmaSnapshots);
    spiResult=-5;assert(PaganiInput::readTouch(&device,bytes,4)==-5 && dmaSnapshots==1);
    const uint32_t expectedDma[]={4,0,2,1};assert(!memcmp(dmaSnapshot,expectedDma,sizeof expectedDma));
    spiResult=0;
    // Build 54: a successful DMA read containing zeros must retain evidence even though TouchComm rejects it.
    PaganiInput zero;zero.spi.use_dma=1;spiRxByte=0;
    s3910 chip={};chip.bus={&zero,PaganiInput::readTouch,PaganiInput::writeTouch,[](void *,unsigned){}};
    assert(s3910_start(&chip)==-71 && !chip.version && !chip.detail && !strcmp(chip.stage,"header"));
    const uint32_t zeroRead[]={4,0,13,1,0x40,4,4,0xc800};
    assert(readSnapshots==2 && !memcmp(readSnapshot,zeroRead,sizeof zeroRead));
    assert(zero.firstReadCaptured && zero.dmaExposed && !zero.spi.snap_valid);
    spiRxByte=0xa5;
    PaganiInput shortRead;uint8_t byte=0;
    assert(!PaganiInput::readTouch(&shortRead,&byte,1) && readSnapshot[0]==1 && readSnapshot[1]==0xa5);
    PaganiInput failed;spiResult=-110;unsigned before=statusReads,timingBefore=timingReads;
    assert(PaganiInput::readTouch(&failed,bytes,4)==-110 && !failed.firstReadCaptured);
    assert(statusReads==before && timingReads==timingBefore && failed.pins[5].reads==1 && failed.pins[4].reads==0);
    spiResult=0;
}

static void descriptor(const uint8_t *bytes,size_t length,bool touch) {
    unsigned bits[4]={},id=0,size=0,count=0,page=0,usage=0,depth=0;
    std::vector<std::pair<unsigned,unsigned>> applications;
    for (size_t offset=0;offset<length;) {
        uint8_t tag=bytes[offset++];assert(tag!=0xfe);
        unsigned n=tag&3;if (n==3) n=4;
        assert(n<=length-offset);
        unsigned value=0;for (unsigned i=0;i<n;++i) value|=(unsigned)bytes[offset++]<<(8*i);
        switch (tag&0xfc) {
            case 0x04:page=value;break;
            case 0x08:usage=value;break;
            case 0x74:size=value;break;
            case 0x94:count=value;break;
            case 0x84:id=value;assert(id && id<4);break;
            case 0x80:assert(id);bits[id]+=size*count;break;
            case 0xa0:if (!depth) {assert(value==1);applications.emplace_back(page,usage);}++depth;break;
            case 0xc0:assert(depth);--depth;break;
        }
    }
    assert(!depth && bits[1]==(touch?0:64) && bits[2]==(touch?0:8) && bits[3]==(touch?61*8:0));
    assert(applications.size()==1 && applications[0]==(touch?std::make_pair(13u,4u):std::make_pair(1u,6u)));
}

static void edgeContacts() {
    bool state[256]={};
    auto edge=[&](unsigned id,bool changed,bool down,int x,int y) {
        return pagani_touch_edge(state,id,changed,down,x,y,2048,1024);
    };
    assert(edge(2,true,true,32768,65535));
    assert(edge(2,false,true,32768,32768));
    assert(edge(2,true,false,32768,32768) && !state[2]);
    assert(!edge(2,true,true,32768,32768));
    assert(!edge(2,false,true,0,0)); // Moving to an edge does not create an edge-origin gesture.
    assert(!edge(2,true,false,0,0));
    for (auto point:std::vector<std::pair<int,int>>{{0,32768},{65536,32768},{32768,0},{32768,65536}}) {
        assert(edge(255,true,true,point.first,point.second));
        assert(edge(255,true,false,point.first,point.second) && !state[255]);
    }
    assert(!edge(256,true,true,0,0));
    assert(!edge(0,true,true,-1,0) && !edge(0,true,true,65537,0));
    assert(!edge(0,true,true,2048,1024) && !edge(0,true,true,65536-2048,65536-1024));
}

struct Tcm {
    std::vector<std::vector<uint8_t>> reads,writes;
    size_t next=0;
    static int read(void *ctx,uint8_t *p,size_t n) {
        auto &m=*static_cast<Tcm *>(ctx);
        if (m.next>=m.reads.size()) return -110;
        const auto &r=m.reads[m.next++];assert(r.size()==n);memcpy(p,r.data(),n);return 0;
    }
    static int write(void *ctx,const uint8_t *p,size_t n) {
        static_cast<Tcm *>(ctx)->writes.emplace_back(p,p+n);return 0;
    }
    static void sleep(void *,unsigned) {}
    void message(uint8_t code,const std::vector<uint8_t> &body={}) {
        reads.push_back({0xa5,code,(uint8_t)body.size(),(uint8_t)(body.size()>>8)});
        if (body.empty()) return;
        std::vector<uint8_t> next{0xa5,3};next.insert(next.end(),body.begin(),body.end());
        next.insert(next.end(),5,0x5a);reads.push_back(next);
    }
    s3910 device() {s3910 d={};d.bus={this,read,write,sleep};return d;}
    void startup(bool corruptReadback=false) {
        std::vector<uint8_t> id(24),app(38);
        id[1]=1;id[23]=2;message(0x10,id);
        app[12]=16;app[32]=0;app[33]=0x4c;app[34]=0;app[35]=0xa5;app[36]=10;
        message(1);message(1,app);message(1);message(1);message(1,{2,0});message(1);
        std::vector<uint8_t> format={0x18,8,1,6,8,7,8,8,16,9,16,3,0,0,0,0};
        if (corruptReadback) format[0]=0;
        message(1,format);if (!corruptReadback) message(1);
    }
};

// Independent table CRC and fixed IDENTIFY bytes check the production bitwise encoder.
static uint8_t headerCrc(const uint8_t *p,unsigned bits) {
    static const uint16_t table[]={0,268,536,788,1072,1340,1576,1828,2144,2412,2680,2932,3152,3420,3656,3908};
    uint16_t r=0xfc;
    for (;bits>8;bits-=8) {
        r^=*p++;
        r=(uint16_t)((r<<4)^table[(r>>4)&15]);r=(uint16_t)((r<<4)^table[(r>>4)&15]);
    }
    unsigned x=*p;
    while (bits--) {if (x&0x80) r^=0x80;x<<=1;r<<=1;if (r&0x100) r^=12;}
    return (r>>2)&63;
}
static uint16_t packetCrc(const std::vector<uint8_t> &bytes) {
    uint16_t crc=0xffff;
    for (auto byte:bytes) {
        uint16_t x=(crc>>8)^byte;x^=x>>4;
        crc=(uint16_t)((crc<<8)^(x<<12)^(x<<5)^x);
    }
    return crc;
}
struct Tcm2 : Tcm {
    static std::vector<uint8_t> header(uint8_t code,unsigned length) {
        std::vector<uint8_t> h={code,(uint8_t)length,(uint8_t)(length>>8),0};
        h[3]=headerCrc(h.data(),26);return h;
    }
    void response(uint8_t code,const std::vector<uint8_t> &body={},unsigned cap=64) {
        reads.push_back(header(code,body.size()));
        for (size_t off=0;off<body.size();) {
            size_t n=body.size()-off;if (n>cap-6) n=cap-6;
            auto packet=header(code,n);packet.insert(packet.end(),body.begin()+off,body.begin()+off+n);
            uint16_t crc=packetCrc(packet);packet.push_back(crc>>8);packet.push_back(crc);
            reads.push_back(packet);off+=n;
        }
    }
    void startup(unsigned maxRead=64,unsigned maxWrite=64) {
        reads.push_back({4,0,0,0x3c});
        std::vector<uint8_t> id(26),app(38);
        id[1]=1;id[22]=(uint8_t)maxWrite;id[23]=(uint8_t)(maxWrite>>8);
        id[24]=(uint8_t)maxRead;id[25]=(uint8_t)(maxRead>>8);response(0x10,id,256);
        app[12]=16;app[33]=0x4c;app[35]=0xa5;app[36]=10;
        response(1);response(1,app,maxRead);response(1);response(1);response(1,{2,0},maxRead);
        for (unsigned off=0;off<16;) {off+=maxWrite-6;response(off<16?7:1);}
        response(1,{0x18,8,1,6,8,7,8,8,16,9,16,3,0,0,0,0},maxRead);response(1);
    }
    void checkPackets() const {
        for (const auto &p:writes) {
            assert(p.size()>=4 && !headerCrc(p.data(),32));
            unsigned length=p[1]+(p[2]<<8);assert(p.size()==length+4+(length?2:0));
            if (length) assert(!packetCrc(p));
        }
    }
};

static void touchV2() {
    Tcm2 bus;bus.startup();auto d=bus.device();assert(!s3910_start(&d) && d.ready && d.version==2);
    assert(d.max_read==64 && d.max_write==64 && bus.next==bus.reads.size());
    assert(bus.writes[0]==std::vector<uint8_t>({2,0,0,0x19}));bus.checkPackets();
    s3910_frame frame={};bus.response(0x11,{1,2,1,0,0x26,0,0x52});
    assert(s3910_poll(&d,&frame)==1 && frame.contact[2].active && bus.writes[bus.writes.size()-2][0]==0x0a);
    bus.checkPackets();
    bus.response(0xff);assert(s3910_poll(&d,&frame)==-5 && !strcmp(d.stage,"reply"));
    Tcm2 chunks;chunks.startup(16,16);auto fragmented=chunks.device();assert(!s3910_start(&fragmented));
    std::vector<uint8_t> config;bool setting=false;
    for (const auto &p:chunks.writes) {
        assert(p.size()<=16);
        if (p[0]==0x26) setting=true;
        if (setting && (p[0]==0x26 || p[0]==1)) config.insert(config.end(),p.begin()+4,p.end()-2);
        if (p[0]==0x25) setting=false;
    }
    assert(config==std::vector<uint8_t>({0x18,8,1,6,8,7,8,8,16,9,16,3,0,0,0,0}));chunks.checkPackets();
    Tcm2 retry;retry.startup();retry.reads.insert(retry.reads.begin()+1,Tcm2::header(8,0));
    auto repeated=retry.device();assert(!s3910_start(&repeated) && retry.writes[0]==retry.writes[1]);retry.checkPackets();
    Tcm2 pending;pending.startup();pending.reads.insert(pending.reads.begin()+1,Tcm2::header(7,0));
    auto delayed=pending.device();assert(!s3910_start(&delayed) && pending.writes[1][0]==0x0a);pending.checkPackets();
    Tcm2 crc;crc.startup();auto damaged=crc.reads[2];damaged.back()^=1;
    crc.reads.insert(crc.reads.begin()+2,damaged);auto recovered=crc.device();assert(!s3910_start(&recovered));
    assert(crc.writes[1][0]==7 && crc.writes[2][0]==8);crc.checkPackets();
    Tcm2 fail;fail.startup();fail.reads[2]=damaged;fail.reads.insert(fail.reads.begin()+2,4,damaged);
    auto rejected=fail.device();assert(s3910_start(&rejected)==-74 && !rejected.ready);
    assert(!strcmp(rejected.stage,"body CRC") && fail.writes.size()==6);
    Tcm2 badHeader;badHeader.startup();badHeader.reads[1][3]^=1;auto broken=badHeader.device();
    assert(s3910_start(&broken)==-74 && !strcmp(broken.stage,"header") && badHeader.writes.size()==1);
    Tcm2 oversized;oversized.reads={{4,0,0,0x3c},Tcm2::header(0x10,513)};auto large=oversized.device();
    assert(s3910_start(&large)==-90 && oversized.writes.size()==1);
    Tcm2 raw;raw.startup();Tcm2 queued;queued.response(0x23,std::vector<uint8_t>(518),64);
    raw.reads.insert(raw.reads.begin()+3,queued.reads.begin(),queued.reads.end());auto drained=raw.device();
    assert(!s3910_start(&drained) && drained.ready && raw.next==raw.reads.size());raw.checkPackets();
    for (bool readLimit:{false,true}) {
        Tcm2 limit;limit.startup();auto &id=limit.reads[2];id.resize(id.size()-2);
        id[readLimit?28:26]=6;uint16_t crc=packetCrc(id);id.push_back(crc>>8);id.push_back(crc);
        auto invalid=limit.device();assert(s3910_start(&invalid)==-90 && !invalid.ready);
        assert(!strcmp(invalid.stage,readLimit?"read limit":"write limit"));
    }
    Tcm floating;floating.reads={{0xff,0xff,0xff,0xff}};auto absent=floating.device();
    assert(s3910_start(&absent)==-71 && !absent.version && floating.writes.empty());
}
struct MMIO {
    std::map<uint32_t,uint32_t> regs;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    bool complete=true,dsi=false,spmi=false;
    unsigned waited=0;
    static uint32_t read(void *ctx,uint32_t reg) {return static_cast<MMIO *>(ctx)->regs[reg];}
    static void write(void *ctx,uint32_t reg,uint32_t value) {
        auto &m=*static_cast<MMIO *>(ctx);m.writes.emplace_back(reg,value);
        if (m.dsi && reg==0x110) m.regs[reg]=(value&0xa222aa02)|(m.regs[reg]&0x55555555&~value);
        else m.regs[reg]=value;
        if (m.dsi && reg==0x90 && m.complete) m.regs[0x110]|=1;
        if (m.spmi && (reg&31)==0) m.regs[reg+8]=m.complete?1:0;
    }
    static void delay(void *ctx,unsigned us) {static_cast<MMIO *>(ctx)->waited+=us;}
    peripheral_io io() {return {this,read,write,delay};}
};
struct Rsc : MMIO {
    unsigned major,wait,control,status,enable,msg,stride,pending=~0u;
    uint32_t ignored=~0u;
    bool partial=false;
    explicit Rsc(unsigned version=3):major(version),wait(version==2?0x10:0x20),
        control(wait+4),status(wait+8),enable(wait+12),msg(version==2?0x30:0x34),stride(version==2?20:24) {
        regs[0]=version<<16;regs[0x0c]=(3u<<12)|(16u<<27);regs[0xd00]=0x87;
        for (unsigned i=0;i<3;++i) regs[0xd00+i*0x2a0+status]=1;
    }
    static void write(void *ctx,uint32_t reg,uint32_t value) {
        auto &m=*static_cast<Rsc *>(ctx);m.writes.emplace_back(reg,value);
        if (reg==m.ignored) return;
        if (reg==0xd08) {m.regs[0xd04]&=~value;return;}
        m.regs[reg]=value;
        for (unsigned i=0;i<3;++i) {
            unsigned b=0xd00+i*0x2a0;
            if (reg==b+m.control && (value&(1u<<24))) {
                assert(value==(1u<<24|1u<<16) && m.regs[b+m.enable]==7 && m.regs[b+m.wait]==7);
                m.pending=b;m.regs[b+m.status]=0;
                for (unsigned c=0;c<3;++c) m.regs[b+m.msg+12+c*m.stride]=0;
            }
        }
    }
    static void delay(void *ctx,unsigned us) {
        auto &m=*static_cast<Rsc *>(ctx);m.waited+=us;
        if (m.pending==~0u || !m.complete) return;
        m.regs[m.pending+m.status]=1;
        for (unsigned c=0;c<3;++c) m.regs[m.pending+m.msg+12+c*m.stride]=(m.partial && c==2)?0x100:0x10100;
        m.regs[0xd04]|=1u<<((m.pending-0xd00)/0x2a0);m.pending=~0u;
    }
    peripheral_io io() {return {this,read,write,delay};}
};
static void dbEntry(MMIO &db,unsigned at,const char *name,uint32_t address) {
    db.regs[at]=db.regs[at+4]=0;
    for (unsigned i=0;name[i];++i) db.regs[at+(i/4)*4]|=(uint32_t)(uint8_t)name[i]<<(8*(i%4));
    db.regs[at+16]=address;
}
static MMIO commandDb() {
    MMIO db;db.regs[4]=0x0c0330db;db.regs[8]=4;db.regs[12]=2u<<16;
    dbEntry(db,144,"ldob4",0x40800);dbEntry(db,168,"ldob16",0x41900);return db;
}
static uint32_t powerSnapshot[7];
static unsigned powerSnapshots,powerWaited;
void boot_progress_touch_power(uint32_t address,uint32_t hardware,uint32_t tcs,uint32_t state,
                               uint32_t cmd0,uint32_t cmd1,uint32_t cmd2) {
    const uint32_t values[]={address,hardware,tcs,state,cmd0,cmd1,cmd2};
    memcpy(powerSnapshot,values,sizeof values);++powerSnapshots;
}
static Rsc *powerRsc;
static MMIO *powerDb;
struct PowerWindow {
    peripheral_io mapped={};
    bool map(uint64_t address,unsigned size) {
        if (address==PAGANI_TOUCH_RSC) {assert(size==PAGANI_TOUCH_RSC_SIZE);mapped=powerRsc->io();}
        else {assert(address==PAGANI_CMD_DB && size==PAGANI_CMD_DB_SIZE);mapped=powerDb->io();}
        return true;
    }
    peripheral_io io() {return mapped;}
};
static void IOSleep(unsigned ms) {powerWaited+=ms;}
struct PowerHarness {
    PowerWindow rsc,cmddb;uint32_t sample=0;
    const char *phase="";
    void observe(const char *value) {phase=value;}
#include "PaganiTouchPower.inc"
};
static void power() {
    MMIO db=commandDb();auto d=db.io();uint32_t address=0;
    assert(!rpmh_vrm_address(&d,PAGANI_CMD_DB_SIZE,PAGANI_TOUCH_VDD,&address) && address==0x40800);
    assert(!rpmh_vrm_address(&d,PAGANI_CMD_DB_SIZE,PAGANI_TOUCH_VCC,&address) && address==0x41900);
    assert(rpmh_vrm_address(&d,PAGANI_CMD_DB_SIZE,"L4B",&address)==-19 && db.writes.empty());
    db.regs[12]=3u<<16;dbEntry(db,192,"ldob4",0x40800);
    assert(rpmh_vrm_address(&d,PAGANI_CMD_DB_SIZE,"ldob4",&address)==-71);
    db.regs[12]=0xffff0000;assert(rpmh_vrm_address(&d,200,"ldob4",&address)==-71);
    db=commandDb();db.regs[160]=0x30800;assert(rpmh_vrm_address(&d,PAGANI_CMD_DB_SIZE,"ldob4",&address)==-71);
    db.regs[4]=0;assert(rpmh_vrm_address(&d,PAGANI_CMD_DB_SIZE,"ldob16",&address)==-19);
    for (unsigned major: {2u,3u}) {
        Rsc m(major);auto io=m.io();rpmh_result result={};
        assert(!sm8750_rpmh_vrm_enable(&io,0x40800,PAGANI_TOUCH_VDD_MV,&result));
        assert(result.tcs==0 && result.status==1 && result.command[2]==0x10100 && m.regs[0xd00]==0x87);
        assert(!m.regs[0xd04] && !m.regs[0xd00+m.control] && !m.regs[0xd00+m.enable] && !m.regs[0xd00+m.wait]);
        for (unsigned i=0;i<3;++i) {
            unsigned at=0xd00+m.msg+i*m.stride;
            assert(m.regs[at]==0x10108);
            assert(m.regs[at+4]==0x40800+(i==0?0:i==1?8:4));
            assert(m.regs[at+8]==(i==0?1800u:i==1?7u:1u));
        }
    }
    Rsc busy;auto io=busy.io();rpmh_result result={};busy.regs[0xd00+busy.control]=1;
    assert(!sm8750_rpmh_vrm_enable(&io,0x41900,3100,&result) && result.tcs==1);
    for (auto [reg,value]:busy.writes) { (void)value;assert(reg<0xd10 || reg>=0xfa0); }
    for (unsigned i=0;i<3;++i) busy.regs[0xd00+i*0x2a0+busy.control]=1;
    busy.writes.clear();assert(sm8750_rpmh_vrm_enable(&io,0x41900,3100,&result)==-16 && busy.writes.empty());
    Rsc unknown(4);io=unknown.io();assert(sm8750_rpmh_vrm_enable(&io,0x41900,3100,&result)==-95 && unknown.writes.empty());
    Rsc fault;io=fault.io();fault.ignored=0xd00+fault.msg+8;
    assert(sm8750_rpmh_vrm_enable(&io,0x40800,1800,&result)==-5 && fault.pending==~0u && fault.regs[0xd00]==0x87);
    for (bool partial: {false,true}) {
        Rsc stalled;io=stalled.io();stalled.complete=partial;stalled.partial=partial;
        assert(sm8750_rpmh_vrm_enable(&io,0x40800,1800,&result)==-110 && stalled.waited==2000000);
        assert(stalled.regs[0xd00+stalled.enable]==7 && stalled.regs[0xd00]==0x86);
        assert(stalled.regs[0xd00+stalled.control]==(1u<<24|1u<<16));
    }
    Rsc mapped;db=commandDb();powerRsc=&mapped;powerDb=&db;PowerHarness device;
    assert(!device.setupTouchPower() && powerWaited==200 && !powerSnapshots);
    assert(mapped.regs[0xd00+mapped.msg+4]==0x41900 && mapped.regs[0xd00+mapped.msg+8]==3100);
    mapped.writes.clear();dbEntry(db,168,"other",0x41900);
    assert(device.setupTouchPower()==-19 && mapped.writes.empty()); // Validate both resources before enabling either rail.
    db=commandDb();db.regs[184]=0x40800;
    assert(device.setupTouchPower()==-71 && mapped.writes.empty()); // Never apply 3.1 V to the 1.8 V resource.
    db=commandDb();mapped.complete=false;
    assert(device.setupTouchPower()==-110 && powerSnapshots==1 && powerSnapshot[0]==0x40800);
    assert(!strcmp(device.phase,"power VDD") && powerWaited==200); // No second rail or reset wait after failure.
}
static void spiTiming() {
    MMIO m;auto io=m.io();uint32_t readback=0;
    m.regs[0x278]=0xfffff;
    assert(!sm8750_spi_timing(&io,PAGANI_TOUCH_CS_DELAY,PAGANI_TOUCH_WORD_DELAY,&readback));
    assert(m.regs[0x278]==0xc800 && readback==0xc800 && m.writes.size()==1);
    m.writes.clear();assert(sm8750_spi_timing(&io,1024,0,&readback)==-22 && m.writes.empty());
    assert(sm8750_spi_timing(&io,0,1024,&readback)==-22 && m.writes.empty());
    io.write=[](void *,uint32_t,uint32_t){};m.regs[0x278]=0;
    assert(sm8750_spi_timing(&io,50,0,&readback)==-5 && readback==0);
}
static void touch() {
    Tcm bus;bus.startup();auto d=bus.device();assert(s3910_start(&d)==0 && d.ready);
    assert(d.max_x==19456 && d.max_y==42240 && bus.next==bus.reads.size());
    assert(bus.writes[3]==std::vector<uint8_t>({0x24,3,0,0x11,2,0}));
    assert(bus.writes.back()==std::vector<uint8_t>({5,1,0,0x11}));
    assert(bus.writes[5].size()==19 && bus.writes[5][0]==0x26);
    Tcm bad;bad.startup(true);auto rejected=bad.device();assert(s3910_start(&rejected)==-71 && !rejected.ready);
    assert(bad.writes.back()[0]==0x25); // No touch enable after an unconfirmed format.
    assert(rejected.command==0x25 && !strcmp(rejected.stage,"format") && rejected.detail==0x06010800);
    Tcm crc;crc.startup();crc.reads[1][27]=0x34;crc.reads[1][28]=0x12;
    auto cd=crc.device();assert(s3910_start(&cd)==0 && cd.crc);
    assert(crc.writes[1]==std::vector<uint8_t>({0x20,0,0,0x5a,0x4a}));
    Tcm raw;raw.startup();
    std::vector<uint8_t> first(514,0),last(13,0x5a);first[0]=last[0]=0xa5;first[1]=last[1]=3;
    raw.reads.insert(raw.reads.begin()+2,{{0xa5,0x23,6,2},first,last});
    auto rd=raw.device();assert(!s3910_start(&rd) && rd.ready);
    Tcm oversized;oversized.reads={{0xa5,0x10,1,2}};auto od=oversized.device();
    assert(s3910_start(&od)==-90 && oversized.writes.empty());
    Tcm version;version.reads={{0x10,0,0,0}};auto vd=version.device();assert(s3910_start(&vd)==-71);
    assert(!strcmp(vd.stage,"header") && vd.detail==0x10 && vd.command==0);
    Tcm continuation;continuation.startup();continuation.reads[1][1]=4;
    auto failedBody=continuation.device();assert(s3910_start(&failedBody)==-71);
    assert(!strcmp(failedBody.stage,"continued") && failedBody.detail==0x010004a5 && continuation.writes.empty());
    Tcm app;app.startup();app.reads[4][4]=1;auto failedApp=app.device();assert(s3910_start(&failedApp)==-71);
    assert(failedApp.command==0x20 && !strcmp(failedApp.stage,"app info") && failedApp.detail==1);
    s3910_frame frame={},old={};
    const uint8_t sample[]={3, 2,1,0,0x26,0,0x52, 7,2,0,0x4c,0,0xa5, 9,6,0,0,0,0};
    assert(s3910_decode(sample,sizeof sample,19456,42240,&frame)==0);
    assert(frame.contact[2].active && frame.contact[2].x==16383 && frame.contact[2].y==16284);
    assert(frame.contact[7].active && frame.contact[7].x==32767 && !frame.contact[9].active);
    uint8_t report[62];s3910_hid_report(&frame,&old,report);
    assert(report[0]==3 && report[61]==2 && report[1]==3 && report[2]==2 && report[8]==7);
    old=frame;frame={};frame.contact[0]={1,2,1};s3910_hid_report(&frame,&old,report);
    assert(report[61]==3 && report[1]==3 && report[7]==2 && report[13]==2); // Two explicit releases plus new contact.
    const uint8_t duplicate[]={2,0,1,0,0,0,0,0,1,0,0,0,0};
    assert(s3910_decode(duplicate,sizeof duplicate,10,10,&frame)==-71);
    assert(s3910_decode(sample,sizeof sample-1,19456,42240,&frame)==-71);
    const uint8_t outOfRange[]={1,10,1,0,0,0,0};assert(s3910_decode(outOfRange,7,10,10,&frame)==-71);
    for (unsigned n=0;n<10;++n) old.contact[n]={10,20,1};
    frame={};s3910_hid_report(&frame,&old,report);assert(report[61]==10);
    for (unsigned n=0;n<10;++n) assert(report[1+6*n]==2 && report[2+6*n]==n);
    bus.message(0x10,std::vector<uint8_t>(24));assert(s3910_poll(&d,&frame)==-104 && !d.ready);
}
static void buttons() {
    assert(pagani_buttons(0,1)==0 && pagani_buttons(0x80,1)==2);
    assert(pagani_buttons(0x40,1)==4 && pagani_buttons(0,0)==8 && pagani_buttons(0xc0,0)==14);
    pagani_button_filter f={};
    assert(pagani_buttons_filter(&f,1,2,0)==0);
    assert(pagani_buttons_filter(&f,1,0,16)==0);
    assert(pagani_buttons_filter(&f,1,2,32)==0);
    assert(pagani_buttons_filter(&f,1,2,63)==0);
    assert(pagani_buttons_filter(&f,1,2,64)==2);
    assert(pagani_buttons_filter(&f,1,2,10000)==2); // Long press remains asserted.
    assert(pagani_buttons_filter(&f,0,2,10001)==0); // A bus failure releases it.
    assert(pagani_buttons_filter(&f,1,2,10002)==0);
    assert(pagani_buttons_filter(&f,1,2,10034)==2);
    assert(pagani_buttons_filter(&f,1,0,10035)==2);
    assert(pagani_buttons_filter(&f,1,0,10067)==0);
}
static void buses() {
    MMIO core,cnfg,obs;auto c=core.io(),f=cnfg.io(),o=obs.io();
    core.regs[0]=0x70000000;core.regs[4]=3;core.regs[0x2000]=0x1300;core.regs[0x2008]=0x1300;
    cnfg.regs[0]=2;cnfg.regs[8]=0;assert(spmi_find(&c,&f,0x13,0)==2);
    assert(spmi_find(&c,&f,0x18d,0)==-19);assert(core.writes.empty() && cnfg.writes.empty());
    obs.spmi=true;obs.regs[0x58]=0xc0;uint8_t value=0;
    assert(!spmi_read8(&o,0,2,0x10,&value) && value==0xc0);
    assert(obs.writes.back()==std::make_pair(0x40u,0x08000100u));
    obs.complete=false;assert(spmi_read8(&o,0,2,0x10,&value)==-110 && obs.waited==1000);
    core.regs[0]=0x80000000;assert(spmi_find(&c,&f,0x13,0)==-19);
    MMIO gcc;auto g=gcc.io();gcc.regs[0x52008]=0x80000000;unsigned hz=0,sel=99;
    assert(!sm8750_touch_clock(&g,&hz,&sel) && hz==19200000 && sel==0);
    assert(gcc.regs[0x52008]==0x843c0000);
    gcc.regs[0x183e0]=0x300;assert(sm8750_touch_clock(&g,&hz,&sel)==-95);
    gcc.regs[0x2315c]=0x80000000;assert(sm8750_touch_clock(&g,&hz,&sel)==-110);
}
static void panel() {
    MMIO m;m.dsi=true;m.regs[4]=1;m.regs[0x110]=0x20010102;m.regs[0x3c]=0xa5;m.regs[0x390]=2;m.regs[0x4c]=32;
    auto io=m.io();assert(!dsi_panel_command(&io,0x51,0x599));
    std::vector<uint32_t> fifo;for (auto [reg,value]:m.writes) if (reg==0x17c) fifo.push_back(value);
    assert(fifo==std::vector<uint32_t>({0xc0390003,0xff059951}));
    assert(m.regs[0x3c]==0xa5 && m.regs[0x390]==2 && m.regs[0x4c]==32 && !m.regs[0x15c]);
    assert((m.regs[0x110]&0x10100)==0x10100); // Do not acknowledge unrelated frame completions.
    m.writes.clear();assert(!dsi_panel_command(&io,0x28,0));
    assert(m.writes[2]==std::make_pair(0x17cu,0x80050028u));
    m.writes.clear();assert(dsi_panel_command(&io,0x51,0x10000)==-22 && m.writes.empty());
    m.regs[8]=2;assert(dsi_panel_command(&io,0x51,1)==-16 && m.writes.empty());
    m.regs[8]=0;m.complete=false;assert(dsi_panel_command(&io,0x51,1)==-110);
    assert(m.regs[0x15c]==0x30006); // No reset while the engine may still own the command.
    m.writes.clear();assert(dsi_panel_command(&io,0x51,1)==-19 && m.writes.empty());
}
int main() {
    spiRead();touchV2();
    descriptor(buttonsDescriptor,sizeof buttonsDescriptor,false);descriptor(touchDescriptor,sizeof touchDescriptor,true);
    touch();edgeContacts();buttons();buses();panel();power();spiTiming();puts("PASS: SPI, TouchComm v1/v2, touchscreen identity, HID contacts/edge lifecycle, buttons, RPMh and timing");
}

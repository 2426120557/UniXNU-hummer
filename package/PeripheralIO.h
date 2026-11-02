// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register facts: OnePlus gcc-sun.c, spmi-pmic-arb.c and Qualcomm dsi_ctrl_hw_cmn.c.
#pragma once
#include <stdint.h>

struct peripheral_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*delay)(void *,unsigned);
};
static inline uint32_t peripheral_read(const struct peripheral_io *io,uint32_t reg) {return io->read(io->context,reg);}
static inline void peripheral_write(const struct peripheral_io *io,uint32_t reg,uint32_t value) {io->write(io->context,reg,value);}

#define SM8750_SPI_DELAY_COUNTERS 0x278
static inline int sm8750_spi_timing(const struct peripheral_io *se,unsigned cs,unsigned words,uint32_t *readback) {
    if (cs>0x3ff || words>0x3ff || !readback) return -22;
    uint32_t value=(cs<<10)|words;
    peripheral_write(se,SM8750_SPI_DELAY_COUNTERS,value);
    *readback=peripheral_read(se,SM8750_SPI_DELAY_COUNTERS);
    return (*readback&0xfffff)==value?0:-5;
}

// Observer reads need no peripheral write ownership; prefer the local EE's mapping.
static inline int spmi_find(const struct peripheral_io *core,const struct peripheral_io *cnfg,unsigned ppid,unsigned ee) {
    uint32_t version=peripheral_read(core,0),count=peripheral_read(core,4)&0x7ff;
    if (version<0x70000000 || version>=0x80000000 || !count || count>1024 || ppid>0xfff || ee>7) return -19;
    int found=-19;
    for (unsigned i=0;i<count;++i) {
        uint32_t map=peripheral_read(core,0x2000+4*i);
        if (map && ((map>>8)&0xfff)==ppid &&
            (found<0 || (peripheral_read(cnfg,4*i)&7)==ee)) found=(int)i;
    }
    return found;
}
static inline int spmi_read8(const struct peripheral_io *obs,unsigned ee,int apid,uint8_t offset,uint8_t *value) {
    if (ee>7 || apid<0 || apid>=1024 || !value) return -22;
    uint32_t ch=ee*0x8000+(unsigned)apid*0x20;
    peripheral_write(obs,ch,(1u<<27)|((uint32_t)offset<<4));
    for (unsigned n=0;n<1000;++n) {
        uint32_t status=peripheral_read(obs,ch+8);
        if (status&0xe) return -5;
        if (status&1) {*value=(uint8_t)peripheral_read(obs,ch+0x18);return 0;}
        obs->delay(obs->context,1);
    }
    return -110;
}

// Adopt an already configured QUP1 S4 clock; do not change PLLs or DFS policy.
static inline int sm8750_touch_clock(const struct peripheral_io *gcc,unsigned *hz,unsigned *select) {
    const uint32_t branches[]={0x2315c,0x23148,0x23140,0x23144,0x183cc};
    uint32_t vote=peripheral_read(gcc,0x52008);
    peripheral_write(gcc,0x52008,vote|0x043c0000);
    for (unsigned i=0;i<5;++i) {
        unsigned n=0;
        for (;n<200;++n) {
            uint32_t v=peripheral_read(gcc,branches[i]);
            if ((v&2) || !(v&0x80000000)) break;
            gcc->delay(gcc->context,1);
        }
        if (n==200) return -110;
    }
    uint32_t dfs=peripheral_read(gcc,0x183dc+0x14);
    unsigned best=0,index=0;
    for (unsigned i=0;i<((dfs&1)?8u:1u);++i) {
        uint32_t cfg=peripheral_read(gcc,0x183dc+((dfs&1)?0x1c+4*i:4));
        unsigned parent=(cfg>>8)&7;
        unsigned source=parent==0?19200000:parent==1?600000000:parent==6?300000000:0;
        // Fractional M/N modes are refused until their live settings are captured.
        if (!source || ((cfg>>12)&3)) continue;
        unsigned rate=(cfg&31)?(unsigned)((uint64_t)source*2/((cfg&31)+1)):source;
        if (rate && (!best || rate<best)) {best=rate;index=i;}
    }
    if (!best) return -95;
    *hz=best;*select=index;return 0;
}

// Only DCS display on/off and 16-bit brightness; preserve the firmware's scanout setup.
static inline int dsi_panel_command(const struct peripheral_io *io,uint8_t command,unsigned level) {
    if ((command!=0x51 && command!=0x28 && command!=0x29) || level>0xffff) return -22;
    if (!(peripheral_read(io,4)&1) || peripheral_read(io,0x15c)) return -19;
    unsigned n;
    for (n=0;n<2000 && (peripheral_read(io,8)&6);++n) io->delay(io->context,10);
    if (n==2000) return -16;
    uint32_t dma=peripheral_read(io,0x3c),dma1=peripheral_read(io,0x390),length=peripheral_read(io,0x4c);
    // Status bits are W1C; keep interrupt enables without acknowledging other clients.
    const uint32_t enable_mask=0xa222aa02;
    uint32_t enables=peripheral_read(io,0x110)&enable_mask;
    peripheral_write(io,0x110,enables|1);
    peripheral_write(io,0x15c,0x30006);
    uint32_t header=command==0x51?0xc0390003:0x80050000u|command;
    peripheral_write(io,0x17c,header);
    peripheral_write(io,0x17c,command==0x51?0xff000051u|((level&255)<<8)|((level>>8)<<16):0);
    peripheral_write(io,0x3c,(dma&~0xc0000000u)|0x14000000);
    peripheral_write(io,0x390,dma1&~2u);
    peripheral_write(io,0x4c,command==0x51?8:4);
    peripheral_write(io,0x90,1);
    for (n=0;n<5000 && !(peripheral_read(io,0x110)&1);++n) io->delay(io->context,10);
    // Do not reset a command still owned by the engine on timeout. Caller disables further requests.
    if (n==5000) return -110;
    peripheral_write(io,0x110,enables|1);
    peripheral_write(io,0x15c,0);
    peripheral_write(io,0x1ec,1);io->delay(io->context,1);peripheral_write(io,0x1ec,0);
    peripheral_write(io,0x3c,dma);peripheral_write(io,0x390,dma1);peripheral_write(io,0x4c,length);
    return 0;
}

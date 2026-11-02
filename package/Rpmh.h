// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register/layout facts: Qualcomm cmd-db.c, rpmh-rsc.c and rpmh-regulator.c.
#pragma once
#include "PeripheralIO.h"

static inline int rpmh_vrm_address(const struct peripheral_io *db,unsigned size,
                                  const char *name,uint32_t *address) {
    if (!db || !db->read || !name || !address || size<144) return -22;
    uint32_t key[2]={0,0};unsigned length=0;
    while (length<8 && name[length]) {
        key[length/4]|=(uint32_t)(uint8_t)name[length]<<(8*(length%4));++length;
    }
    if (!length || (length==8 && name[length])) return -22;
    if (peripheral_read(db,4)!=0x0c0330db) return -19;
    unsigned matches=0;uint32_t found=0;
    for (unsigned i=0;i<8;++i) {
        uint32_t a=peripheral_read(db,8+16*i),b=peripheral_read(db,12+16*i);
        if ((a&0xffff)!=4) continue; // VRM entries only; offsets are relative to data at 144.
        unsigned start=144+(a>>16),count=b>>16;
        if ((start&3) || start>size || count>(size-start)/24) return -71;
        for (unsigned n=0;n<count;++n) {
            unsigned at=start+24*n;
            if (peripheral_read(db,at)!=key[0] || peripheral_read(db,at+4)!=key[1]) continue;
            found=peripheral_read(db,at+16);++matches;
        }
    }
    if (!matches) return -19;
    if (matches!=1 || (found&15) || (found>>16)!=4) return -71;
    *address=found;return 0;
}

struct rpmh_result {
    uint32_t hardware,tcs,status,command[3];
};
static inline int rpmh_write_checked(const struct peripheral_io *io,uint32_t reg,uint32_t value) {
    peripheral_write(io,reg,value);
    for (unsigned i=0;i<100;++i) {
        if (peripheral_read(io,reg)==value) return 0;
        io->delay(io->context,1);
    }
    return -5;
}

// SM8750 apps DRV-2: DT assigns TCS 0..2 to ACTIVE, with base 0xd00 and stride 0x2a0.
// Caller owns these requests serially; sleep/wake TCSes and other IRQ bits are preserved.
struct rpmh_command {uint32_t address,value;};
static inline int sm8750_rpmh_write(const struct peripheral_io *rsc,const struct rpmh_command *commands,
                                  unsigned count,struct rpmh_result *result) {
    if (!rsc || !rsc->read || !rsc->write || !rsc->delay || !result ||
        !commands || !count || count>3) return -22;
    for (unsigned i=0;i<count;++i) {
        unsigned slave=commands[i].address>>16;
        if ((commands[i].address&3) || (slave!=3 && slave!=4 && slave!=5) ||
            (slave==3 && commands[i].value>255) || (slave==4 && commands[i].value>8191) ||
            (slave==5 && (commands[i].value&0x90000000u))) return -22;
    }
    struct rpmh_result initial={0,0xffffffffu,0,{0,0,0}};*result=initial;
    result->hardware=peripheral_read(rsc,0);
    unsigned major=(result->hardware>>16)&255;
    uint32_t config=peripheral_read(rsc,0x0c);
    unsigned ntcs=(config>>12)&63,ncmd=(config>>27)&31;
    if ((major!=2 && major!=3) || ntcs<3 || ntcs>16 || ncmd<3 || ncmd>16) return -95;
    unsigned wait=major==2?0x10:0x20,control=wait+4,status=wait+8,enable=wait+12;
    unsigned msg=major==2?0x30:0x34,stride=major==2?20:24;
    unsigned slot;
    for (slot=0;slot<3;++slot) {
        unsigned b=0xd00+slot*0x2a0;
        if (peripheral_read(rsc,b+control) || peripheral_read(rsc,b+enable) ||
            peripheral_read(rsc,b+wait) || (peripheral_read(rsc,0xd04)&(1u<<slot))) continue;
        if (peripheral_read(rsc,b+status)==1) break;
    }
    if (slot==3) return -16;
    result->tcs=slot;
    unsigned base=0xd00+slot*0x2a0;uint32_t bit=1u<<slot;
    uint32_t irq=peripheral_read(rsc,0xd00);
    int rc=rpmh_write_checked(rsc,0xd00,irq&~bit);
    if (rc) return rc;
    for (unsigned i=0;!rc && i<count;++i) {
        unsigned at=base+msg+i*stride;
        rc=rpmh_write_checked(rsc,at,0x10108);
        if (!rc) rc=rpmh_write_checked(rsc,at+4,commands[i].address);
        if (!rc) rc=rpmh_write_checked(rsc,at+8,commands[i].value);
    }
    const uint32_t mask=(1u<<count)-1;
    if (!rc) rc=rpmh_write_checked(rsc,base+wait,mask);
    if (!rc) rc=rpmh_write_checked(rsc,base+enable,mask);
    if (!rc) rc=rpmh_write_checked(rsc,base+control,1u<<16);
    if (!rc) {
        peripheral_write(rsc,base+control,(1u<<16)|(1u<<24));
        rc=-110;
        for (unsigned n=0;n<2000;++n) {
            rsc->delay(rsc->context,1000);
            result->status=peripheral_read(rsc,base+status);
            unsigned done=result->status==1;
            for (unsigned i=0;i<count;++i) {
                uint32_t v=peripheral_read(rsc,base+msg+12+i*stride);result->command[i]=v;
                done&=v!=0xffffffffu && (v&0x10100)==0x10100;
            }
            if (done) {rc=0;break;}
        }
        // shortcut: retain a timed-out TCS with its IRQ masked; reset/reuse needs hardware qualification.
        if (rc) return rc;
    }
    // Only an untriggered or completed TCS can be returned to the pool.
    if (rpmh_write_checked(rsc,base+control,1u<<16) || rpmh_write_checked(rsc,base+control,0) ||
        rpmh_write_checked(rsc,base+enable,0) || rpmh_write_checked(rsc,base+wait,0)) return -5;
    peripheral_write(rsc,0xd08,bit);
    uint32_t current=peripheral_read(rsc,0xd00);
    if (rpmh_write_checked(rsc,0xd00,(current&~bit)|(irq&bit))) return -5;
    return rc;
}

static inline int sm8750_rpmh_vrm_enable(const struct peripheral_io *rsc,uint32_t address,
                                       unsigned millivolts,struct rpmh_result *result) {
    if ((address&15) || (address>>16)!=4 || !millivolts || millivolts>8191) return -22;
    // Voltage, high-power mode, then enable.
    const struct rpmh_command commands[3]={{address,millivolts},{address+8,7},{address+4,1}};
    return sm8750_rpmh_write(rsc,commands,3,result);
}

// SPDX-License-Identifier: BSD-2-Clause-Patent
// TouchComm wire values: OnePlus synaptics_hbp/tcm; implementation is independent of Linux.
#include "S3910.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p+2)<<16; }
static const uint8_t format[] = {0x18,8, 1, 6,8, 7,8, 8,16, 9,16, 3,0};

static uint8_t crc6(const uint8_t *p,unsigned bits) {
    uint8_t crc=0x3f;
    for (unsigned i=0;i<bits;++i) {
        unsigned bit=((crc>>5)^(p[i/8]>>(7-i%8)))&1;
        crc=(uint8_t)(((crc<<1)&0x3f)^(bit?3:0));
    }
    return crc;
}

static uint16_t crc16(const uint8_t *p,unsigned size) {
    uint16_t crc=0xffff;
    for (unsigned i=0;i<size;++i) {
        crc^=(uint16_t)p[i]<<8;
        for (unsigned b=0;b<8;++b) crc=(uint16_t)((crc<<1)^((crc&0x8000)?0x1021:0));
    }
    return crc;
}

static int write_v2(struct s3910 *s,uint8_t cmd,const uint8_t *data,unsigned length) {
    uint8_t packet[S3910_PAYLOAD_MAX+6];unsigned size=4+length+(length?2:0);
    s->stage="write";s->detail=length;
    if (length>S3910_PAYLOAD_MAX || (length && !data) || (s->max_write && size>s->max_write)) return -90;
    packet[0]=cmd;packet[1]=(uint8_t)length;packet[2]=(uint8_t)(length>>8);
    packet[3]=(uint8_t)((s->sequence++&1)<<6);packet[3]|=crc6(packet,26);
    if (length) {
        memcpy(packet+4,data,length);uint16_t crc=crc16(packet,length+4);
        packet[length+4]=(uint8_t)(crc>>8);packet[length+5]=(uint8_t)crc;
    }
    return s->bus.write(s->bus.context,packet,size);
}

static int response_v2(struct s3910 *s) {
    uint8_t head[4],body[S3910_PAYLOAD_MAX+6];
    s->stage="header";s->detail=0;
    int rc=s->bus.read(s->bus.context,head,sizeof head);if (rc) return rc;
    s->detail=le32(head);if (crc6(head,32)) return -74;
    s->code=head[0];s->length=le16(head+1);
    int discard=s->length>S3910_PAYLOAD_MAX;
    if (discard && (s->ready || s->code!=0x23)) return -90;
    unsigned cap=s->max_read?s->max_read:256;
    if (cap>sizeof body) cap=sizeof body;
    if (cap<=6) {s->stage="read limit";s->detail=cap;return -90;}
    for (unsigned offset=0;offset<s->length;) {
        unsigned size=s->length-offset;if (size>cap-6) size=cap-6;
        for (unsigned retry=0;retry<5;++retry) {
            rc=write_v2(s,retry?8:7,0,0);if (rc) return rc;
            s->stage="continued";s->detail=0;
            rc=s->bus.read(s->bus.context,body,size+6);if (rc) return rc;
            s->detail=le32(body);
            if (!crc6(body,32) && !crc16(body,size+6)) break;
            s->stage="body CRC";
            if (retry==4) return -74;
        }
        if (!discard) memcpy(s->payload+offset,body+4,size);
        offset+=size;
    }
    return 0;
}

static int exchange_v2(struct s3910 *s,uint8_t cmd,const uint8_t *data,unsigned length) {
    uint8_t sequence=s->sequence;
    for (unsigned retry=0;retry<5;++retry) {
        // A requested retransmission keeps the original sequence bit.
        s->sequence=sequence;
        int rc=write_v2(s,cmd,data,length);if (rc) return rc;
        s->bus.sleep_ms(s->bus.context,1);
        rc=response_v2(s);if (rc) return rc;
        if (s->code!=8 && s->code!=0xfe) return 0;
    }
    s->stage="retry";s->detail=s->code;return -5;
}

static int request_v2(struct s3910 *s,uint8_t cmd,const uint8_t *data,unsigned length) {
    unsigned cap=s->max_write?s->max_write:S3910_PAYLOAD_MAX+6,offset=0;
    if (cap>S3910_PAYLOAD_MAX+6) cap=S3910_PAYLOAD_MAX+6;
    s->stage="write limit";s->detail=cap;
    if (length>S3910_PAYLOAD_MAX || (length && (!data || cap<=6)) || cap<4) return -90;
    do {
        unsigned size=length-offset;if (size && size>cap-6) size=cap-6;
        int rc=exchange_v2(s,offset?1:cmd,data?data+offset:0,size);if (rc) return rc;
        if (s->code==0x10 && cmd!=2) {s->stage="restart";return -104;}
        if (s->code<0x10 && s->code!=1 && s->code!=4 && s->code!=7) {
            s->stage="reply";s->detail=s->code;return -5;
        }
        offset+=size;
    } while (offset<length);
    return 0;
}

static int message(struct s3910 *s) {
    if (s->version==2) return exchange_v2(s,0x0a,0,0);
    uint8_t head[4], body[S3910_PAYLOAD_MAX+7];
    s->stage="header";s->detail=0;
    int rc = s->bus.read(s->bus.context, head, sizeof head);
    if (rc) return rc;
    s->detail=le32(head);
    if (head[0] != 0xa5) {
        if (!s->version && s->detail && s->detail!=0xffffffff && !crc6(head,32)) {
            s->version=2;return 0; // Identify through a fresh v2 command, as in the vendor detection path.
        }
        return -71;
    }
    s->version=1;
    s->code = head[1]; s->length = le16(head+2);
    if (s->length > S3910_PAYLOAD_MAX) {
        // A queued HBP raw frame can precede DISABLE_REPORT's response after reset.
        if (s->ready || s->code!=0x23) return -90;
        unsigned left=s->length;
        while (left) {
            unsigned size=left>S3910_PAYLOAD_MAX?S3910_PAYLOAD_MAX:left;
            s->stage="continued";s->detail=0;
            rc=s->bus.read(s->bus.context,body,size+2+(size==left?5:0));
            if (rc) return rc;
            s->detail=le32(body);
            if (body[0]!=0xa5 || body[1]!=3) return -71;
            if (size==left && body[size+2]!=0x5a) {s->stage="padding";s->detail=body[size+2];return -71;}
            left-=size;
        }
        return 0;
    }
    if (!s->length) return 0;
    // The vendor detection reads EOM, optional CRC/RC and trailing EOM together.
    s->stage="continued";s->detail=0;
    rc = s->bus.read(s->bus.context, body, s->length+7);
    if (rc) return rc;
    s->detail=le32(body);
    if (body[0] != 0xa5 || body[1] != 3) return -71;
    if (body[s->length+2] != 0x5a) {s->stage="padding";s->detail=body[s->length+2];return -71;}
    if (s->code==0x10) s->crc=le16(body+s->length+3)!=0x5a5a;
    memcpy(s->payload, body+2, s->length);
    return 0;
}

static int command(struct s3910 *s, uint8_t cmd, const uint8_t *data, unsigned length) {
    uint8_t packet[S3910_PAYLOAD_MAX+5];
    s->command=cmd;s->stage="write";s->detail=length;
    int rc;
    if (s->version==2) {
        rc=request_v2(s,cmd,data,length);
    } else {
        if (length > S3910_PAYLOAD_MAX || (length && !data) ||
            (s->max_write && length+3+2*s->crc > s->max_write)) return -90;
        packet[0]=cmd; packet[1]=(uint8_t)length; packet[2]=(uint8_t)(length>>8);
        if (length) memcpy(packet+3,data,length);
        unsigned size=length+3;
        if (s->crc) {
            uint16_t crc=crc16(packet,size);
            packet[size++]=(uint8_t)crc; packet[size++]=(uint8_t)(crc>>8);
        }
        rc=s->bus.write(s->bus.context,packet,size);
    }
    if (rc) return rc;
    for (unsigned n=0;n<300;++n) {
        if (s->version!=2 || n) {
            s->bus.sleep_ms(s->bus.context,10);
            rc=message(s); if (rc) return rc;
        }
        if (s->code==1 || (cmd==2 && s->code==0x10)) return 0;
        if (s->code==0x10) {s->stage="restart";return -104;} // Controller restarted during a command.
        if (s->code>=0xfe || (s->code && s->code<0x10 && s->code!=4 && s->code!=7)) {
            s->stage="reply";s->detail=s->code;return -5;
        }
    }
    return -110;
}

int s3910_start(struct s3910 *s) {
    if (!s || !s->bus.read || !s->bus.write || !s->bus.sleep_ms) return -22;
    s->ready=0;s->max_write=0;s->max_read=0;s->crc=0;s->version=0;s->sequence=0;
    s->command=0;s->mode=0;s->code=0;s->length=0;
    int rc=message(s);
    if (rc) return rc;
    if (s->code!=0x10) {rc=command(s,2,0,0); if (rc) return rc;}
    s->stage="identify";s->detail=s->length;
    if (s->length<24) return -71;
    s->mode=s->payload[1]; s->max_write=le16(s->payload+22);
    s->detail=s->mode;
    if (s->mode!=1) return -19;
    if (s->version==2 && s->length>=26) s->max_read=le16(s->payload+24);
    uint8_t report=0x23;
    rc=command(s,6,&report,1); if (rc) return rc;
    rc=command(s,0x20,0,0); if (rc) return rc;
    s->stage="app info";s->detail=s->length;
    if (s->length<38) return -71;
    s->detail=le16(s->payload+2);
    if (s->detail) return -71;
    s->config_size=le16(s->payload+12);
    s->max_x=le16(s->payload+32); s->max_y=le16(s->payload+34);
    s->stage="limits";s->detail=(uint32_t)s->config_size<<16|le16(s->payload+36);
    if (!s->max_x || !s->max_y || !le16(s->payload+36) || le16(s->payload+36)>S3910_CONTACTS ||
        s->config_size<sizeof format || s->config_size>S3910_PAYLOAD_MAX) return -71;
    report=0x11;
    rc=command(s,6,&report,1); if (rc) return rc;
    // The vendor disable_hbp path disables raw frames and selects on-controller LBP (2).
    const uint8_t lbp[]={0x11,2,0};
    rc=command(s,0x24,lbp,sizeof lbp); if (rc) return rc;
    const uint8_t key=0x11;
    rc=command(s,0x23,&key,1); if (rc) return rc;
    s->stage="LBP";s->detail=s->length>=2?le16(s->payload):s->length;
    if (s->length!=2 || le16(s->payload)!=2) return -71;
    uint8_t config[S3910_PAYLOAD_MAX]={0}; memcpy(config,format,sizeof format);
    rc=command(s,0x26,config,s->config_size); if (rc) return rc;
    rc=command(s,0x25,0,0); if (rc) return rc;
    s->stage="format";s->detail=s->length>=4?le32(s->payload):s->length;
    if (s->length<sizeof format || memcmp(s->payload,format,sizeof format)) return -71;
    report=0x11; rc=command(s,5,&report,1); if (rc) return rc;
    s->ready=1; return 0;
}

int s3910_decode(const uint8_t *p,size_t length,uint16_t max_x,uint16_t max_y,struct s3910_frame *out) {
    if (!p || !out || !max_x || !max_y || !length || p[0]>S3910_CONTACTS || length!=1u+6u*p[0]) return -71;
    struct s3910_frame frame; memset(&frame,0,sizeof frame); uint16_t seen=0;
    for (unsigned n=0;n<p[0];++n) {
        const uint8_t *c=p+1+n*6; unsigned id=c[0];
        if (id>=S3910_CONTACTS || (seen&(1u<<id))) return -71;
        seen|=(uint16_t)(1u<<id);
        uint16_t x=le16(c+2),y=le16(c+4);
        if (x>max_x || y>max_y) return -71;
        // Finger and glove classifications are contacts; palms never become fingers.
        if (c[1]!=1 && c[1]!=2) continue;
        frame.contact[id].active=1;
        frame.contact[id].x=(uint16_t)((uint32_t)x*32767/max_x);
        frame.contact[id].y=(uint16_t)((uint32_t)y*32767/max_y);
    }
    *out=frame; return 0;
}

int s3910_poll(struct s3910 *s,struct s3910_frame *out) {
    if (!s || !out || !s->ready) return -19;
    if (s->version==2) s->command=0x0a;
    int rc=message(s); if (rc) return rc;
    if (s->code==0x10) {s->ready=0;s->stage="restart"; return -104;}
    if (s->code>=0xfe || (s->code>1 && s->code<0x10 && s->code!=4 && s->code!=7)) {
        s->stage="reply";s->detail=s->code;return -5;
    }
    if (s->code!=0x11) return 0;
    rc=s3910_decode(s->payload,s->length,s->max_x,s->max_y,out);
    if (rc) {s->stage="contacts";s->detail=s->length;}
    return rc?rc:1;
}

void s3910_hid_report(const struct s3910_frame *frame,const struct s3910_frame *previous,uint8_t report[62]) {
    memset(report,0,62); report[0]=3; unsigned count=0;
    for (unsigned id=0;id<S3910_CONTACTS;++id) {
        const struct s3910_contact *c=&frame->contact[id];
        if (!c->active && !previous->contact[id].active) continue;
        const struct s3910_contact *position=c->active?c:&previous->contact[id];
        uint8_t *r=report+1+6*count++;
        r[0]=c->active?3:2; r[1]=(uint8_t)id;
        r[2]=(uint8_t)position->x; r[3]=(uint8_t)(position->x>>8);
        r[4]=(uint8_t)position->y; r[5]=(uint8_t)(position->y>>8);
    }
    report[61]=(uint8_t)count;
}

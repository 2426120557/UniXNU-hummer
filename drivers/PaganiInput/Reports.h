// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PAGANI_INPUT_REPORTS_H
#define PAGANI_INPUT_REPORTS_H
#include <stdint.h>
// HID layouts follow UniXNU/NativeHidInput (5f3a37c), BSD-2-Clause-Patent.
#define KEYBOARD \
    5,1,9,6,0xa1,1,0x85,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1, \
    0x75,1,0x95,8,0x81,2,0x75,8,0x95,1,0x81,3,0x19,0,0x29,0x65, \
    0x15,0,0x25,0x65,0x75,8,0x95,6,0x81,0, \
    0x85,2,5,0x0c,9,0x40,9,0x30,9,0xe9,9,0xea,0x15,0,0x25,1, \
    0x75,1,0x95,4,0x81,2,0x75,4,0x95,1,0x81,3,0xc0
static const uint8_t buttonsDescriptor[]={KEYBOARD};
#define FINGER \
    9,0x22,0xa1,2,9,0x42,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,9,0x47,0x81,2,0x75,6,0x95,1,0x81,3, \
    9,0x51,0x75,8,0x95,1,0x15,0,0x25,9,0x81,2,5,1,9,0x30,0x15,0,0x26,0xff,0x7f,0x75,16,0x95,1,0x81,2,9,0x31,0x81,2,5,0x0d,0xc0,
// The physical touchscreen must publish Digitizer/Touch Screen as its primary usage.
static const uint8_t touchDescriptor[]={5,0x0d,9,4,0xa1,1,0x85,3,
    FINGER FINGER FINGER FINGER FINGER FINGER FINGER FINGER FINGER FINGER
    9,0x54,0x75,8,0x95,1,0x15,0,0x25,10,0x81,2,0xc0};
#undef FINGER
#undef KEYBOARD
// Keep an edge-origin contact marked through its final lift, even after it moves inward.
static inline bool pagani_touch_edge(bool state[256],unsigned id,bool changed,bool down,
                                     int32_t x,int32_t y,uint32_t edgeX,uint32_t edgeY) {
    if (id>=256) return false;
    if (changed && down) state[id]=x>=0 && x<=65536 && y>=0 && y<=65536 &&
        (x<(int32_t)edgeX || x>65536-(int32_t)edgeX || y<(int32_t)edgeY || y>65536-(int32_t)edgeY);
    bool edge=state[id];
    if (changed && !down) state[id]=false;
    return edge;
}
#endif

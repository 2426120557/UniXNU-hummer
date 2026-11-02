// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PAGANI_BUTTONS_H
#define PAGANI_BUTTONS_H
#include <stdint.h>
#include <PaganiPeripherals.h>
struct pagani_button_filter { uint8_t candidate, stable, valid; uint64_t since; };
static inline uint8_t pagani_buttons(uint8_t pon,uint8_t gpio) {
    return (uint8_t)(((pon&PAGANI_PON_POWER)?2:0)|((pon&PAGANI_PON_VOLUME_UP)?4:0)|((gpio&PAGANI_GPIO_VOLUME_DOWN)?0:8));
}
// 32 ms stable state, including release. Invalid reads release all keys and restart debounce.
static inline uint8_t pagani_buttons_filter(struct pagani_button_filter *f,int valid,uint8_t mask,uint64_t ms) {
    if (!valid) {f->valid=0; f->stable=0; return 0;}
    mask&=14;
    if (!f->valid || f->candidate!=mask || ms<f->since) {
        f->candidate=mask; f->since=ms; f->valid=1;
    } else if (ms-f->since>=32) f->stable=mask;
    return f->stable;
}
#endif

// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <PeripheralIO.h>
struct PeripheralWindow {
    IOMemoryMap *mapping=nullptr;
    volatile uint32_t *base=nullptr;
    bool map(uint64_t address,unsigned size) {
        IODeviceMemory *memory=IODeviceMemory::withRange(address,size);
        mapping=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        OSSafeReleaseNULL(memory);
        base=mapping?reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()):nullptr;
        return base!=nullptr;
    }
    void release() {base=nullptr;OSSafeReleaseNULL(mapping);}
    static uint32_t read(void *ctx,uint32_t reg) {
        uint32_t value=static_cast<PeripheralWindow *>(ctx)->base[reg/4];
        __asm__ volatile("dmb sy" ::: "memory");return value;
    }
    static void write(void *ctx,uint32_t reg,uint32_t value) {
        static_cast<PeripheralWindow *>(ctx)->base[reg/4]=value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    static void delay(void *,unsigned us) {IODelay(us);}
    peripheral_io io() {return {this,read,write,delay};}
};

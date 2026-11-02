// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "OriginalHidEventABI.h"
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSUnserialize.h>

// Event fields and edge marking follow ShengHidInput 5f3a37c; the builder qualifies this OS ABI.
namespace pagani_hid {
enum : uint32_t { digitizer=11,xField=0xb0000,yField=0xb0001,indexField=0xb0005,
                 maskField=0xb0007,touchField=0xb0009,touchChanged=2,fromEdgeTip=0x800 };
static uintptr_t dispatchTable[PAGANI_HID_DRIVER_SLOTS+2];
static bool fromEdge[256];
static uint32_t edgeX,edgeY,events,digitizers,edges,lastMask;
static uint64_t lastXY;

static bool installPersonality() {
    static const char xml[] =
        "<array><dict>"
        "<key>IOClass</key><string>IOHIDEventDriver</string>"
        "<key>IOProviderClass</key><string>IOHIDInterface</string>"
        "<key>CFBundleIdentifier</key><string>com.apple.iokit.IOHIDFamily</string>"
        "<key>HIDDefaultBehavior</key><string></string>"
        "<key>Transport</key><string>SPI</string>"
        "<key>IOPropertyMatch</key><dict>"
        "<key>Product</key><string>Pagani S3910</string></dict>"
        "<key>DeviceUsagePairs</key><array><dict>"
        "<key>DeviceUsagePage</key><integer>13</integer>"
        "<key>DeviceUsage</key><integer>4</integer></dict></array>"
        "<key>PrimaryUsagePage</key><integer>13</integer>"
        "<key>PrimaryUsage</key><integer>4</integer>"
        "<key>Built-In</key><true/>"
        "<key>DisplayIntegrated</key><true/>"
        "<key>DigitizerCollectionDispatch</key><true/>"
        "</dict></array>";
    OSObject *object=OSUnserializeXML(xml);
    OSArray *personalities=OSDynamicCast(OSArray,object);
    bool ok=personalities && gIOCatalogue && gIOCatalogue->addDrivers(personalities,false);
    OSSafeReleaseNULL(object);return ok;
}
static uint32_t number(IORegistryEntry *entry,const char *name) {
    OSNumber *value=OSDynamicCast(OSNumber,entry->getProperty(name));
    return value?value->unsigned32BitValue():0xffff;
}
static IOService *findDriver(IORegistryEntry *parent,unsigned depth) {
    if (!depth) return nullptr;
    OSIterator *iterator=parent->getChildIterator(gIOServicePlane);
    if (!iterator) return nullptr;
    IOService *found=nullptr;
    while (OSObject *child=iterator->getNextObject()) {
        IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
        if (!entry) continue;
        if (entry->metaCast("IOHIDEventDriver")) {
            found=OSDynamicCast(IOService,entry);if (found) found->retain();
        } else found=findDriver(entry,depth-1);
        if (found) break;
    }
    iterator->release();return found;
}
template <typename R,typename... A> static R call(void *object,unsigned slot,A... args) {
    return reinterpret_cast<R (*)(void *,A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object,args...);
}
static bool knownEvent(OSObject *event) {
    return event && *reinterpret_cast<uintptr_t **>(event)==pagani_original_hid_event_vtable;
}
static bool mark(OSObject *event,bool &sampled) {
    if (!knownEvent(event) || call<uint32_t>(event,18)!=digitizer) return false;
    uint32_t id=call<int32_t,uint32_t,IOOptionBits>(event,23,indexField,0);
    uint32_t mask=call<int32_t,uint32_t,IOOptionBits>(event,23,maskField,0);
    bool down=call<int32_t,uint32_t,IOOptionBits>(event,23,touchField,0)!=0;
    int32_t x=call<int32_t,uint32_t,IOOptionBits>(event,25,xField,0);
    int32_t y=call<int32_t,uint32_t,IOOptionBits>(event,25,yField,0);
    if (down && !sampled) {
        __atomic_store_n(&lastXY,(uint64_t)(uint32_t)x<<32|(uint32_t)y,__ATOMIC_RELAXED);sampled=true;
    }
    bool edge=pagani_touch_edge(fromEdge,id,(mask&touchChanged)!=0,down,x,y,edgeX,edgeY);
    if (edge) {
        call<void,uint32_t,int32_t,IOOptionBits>(event,24,maskField,(int32_t)(mask|fromEdgeTip),0);
        if (down && (mask&touchChanged)) __atomic_add_fetch(&edges,1,__ATOMIC_RELAXED);
    }
    return edge;
}
static void dispatch(IOService *self,OSObject *event,IOOptionBits options) {
    __atomic_add_fetch(&events,1,__ATOMIC_RELAXED);
    if (knownEvent(event) && call<uint32_t>(event,18)==digitizer) {
        __atomic_add_fetch(&digitizers,1,__ATOMIC_RELAXED);
        OSArray *children=*reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event)+24);
        bool edge=false,sampled=false;
        if (children && OSDynamicCast(OSArray,children) && children->getCount()<=S3910_CONTACTS+1) {
            for (unsigned i=0;i<children->getCount();++i) edge|=mark(children->getObject(i),sampled);
        } else if (!children) edge=mark(event,sampled);
        uint32_t mask=call<int32_t,uint32_t,IOOptionBits>(event,23,maskField,0);
        if (edge) {mask|=fromEdgeTip;call<void,uint32_t,int32_t,IOOptionBits>(event,24,maskField,(int32_t)mask,0);}
        __atomic_store_n(&lastMask,mask,__ATOMIC_RELAXED);
    }
    reinterpret_cast<void (*)(IOService *,OSObject *,IOOptionBits)>(pagani_original_hid_driver_vtable[198])(self,event,options);
}
static bool installDispatch(IOService *driver) {
    uintptr_t *original=*reinterpret_cast<uintptr_t **>(driver);
    if (original!=pagani_original_hid_driver_vtable || dispatchTable[2]) return false;
    memcpy(dispatchTable,original-2,sizeof dispatchTable);
    dispatchTable[198+2]=reinterpret_cast<uintptr_t>(&dispatch);
    // shortcut: keep this single owned driver's table until reboot; add detach synchronization before hot-unplug.
    __atomic_store_n(reinterpret_cast<uintptr_t **>(driver),dispatchTable+2,__ATOMIC_RELEASE);
    return true;
}
}

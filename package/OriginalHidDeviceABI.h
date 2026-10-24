// Generated from the qualified original image; opaque private storage only.
//
// Frozen form of what the engine's scripts/build-native-hid-input.py derives at
// build time from the prelink candidate. The archive step runs *before* the
// prelink, so the derived ABI cannot be produced there; it lives in the device
// package instead. Values were read from this board's own prelink image
// (kernelcache 21H461, slide -0x1000000, the roots of board.toml [kernel.prelink])
// and re-checked against the original IOHIDDevice instructions, handleStart /
// handleReport forwarders, the 207-slot vtable and the 0xd0 instance size.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque168();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque169();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque170();
 virtual OSString *newTransportString() const;
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque172();
 virtual OSString *newProductString() const;
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque174();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque175();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque176();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque177();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque178();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque179();
 virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;
 virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque182();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque183();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque184();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque185();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque186();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque187();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque188();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque189();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque190();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque191();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque192();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque193();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque194();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque195();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque196();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque197();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque198();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque199();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque200();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque201();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque202();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque203();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque204();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque205();
 virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque206();
};
static_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");

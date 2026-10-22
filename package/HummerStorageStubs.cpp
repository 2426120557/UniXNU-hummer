// Diagnostic link stubs for the NativeUfs probe stage (native-ufs-probe=2).
//
// Why these exist at all: NativeUfs.cpp is a single translation unit that also
// derives NativeUfsBlock from IOBlockStorageDevice, and the references below
// come from that class.  XNU 10063 ships no iokit/storage tree at all, so
// IOBlockStorageDevice only exists inside the prelinked
// com.apple.iokit.IOStorageFamily kext - it is not part of our kernel link.
// The same unit also references VerifiedRoot (com.apple root verification) and
// an optional boot-progress observer, neither of which we build yet.
//
// The IOBlockStorageDevice base methods are NOT listed here by hand: the vtable
// of NativeUfsBlock needs every non-overridden virtual of the base class, i.e.
// a list that follows whatever the storage-abi headers of the day declare.
// scripts/build-hummer-storage-stubs.py reads the object's real undefined
// symbols and generates a thunk for each one, so this stays correct by
// construction.  This file only carries the things that cannot be a thunk:
// the vtable storage, the VerifiedRoot data, and the shared refusal function.
//
// Design rules:
//
//   - Nothing is exported.  These definitions exist to satisfy references made
//     by our own dead code (nothing in the probe path constructs
//     NativeUfsBlock).  Prelink binds every kext's undefined symbols against
//     the kernel's symbol table, so an exported stub here would let a real
//     storage kext bind its call to our panic instead of the IOStorageFamily
//     implementation.  Everything is private extern / hidden.
//
//   - Every named object is explicitly `extern` as well as `const`.  At
//     namespace scope C++ gives a const object internal linkage, which would
//     make the definition invisible to NativeUfs.o and leave the link broken.
//
//   - Nothing pretends to succeed.  Every callable path prints and panics.  The
//     VerifiedRoot data is empty by construction (policy count 0), so
//     NativeUfsBlock::configureVerification() takes its "policy count invalid"
//     soft-failure path and the root stays closed rather than half-open.
//
//   - boot_progress_poll is the optional passive observer NativeUfs declares
//     weak, i.e. a no-op is the documented behaviour when it is absent.
//
// When the root path is actually brought up, these must be replaced by a
// deliberate binding to the real IOStorageFamily class in the kernelcache (the
// same technique build-hummer-platform-power.py uses for ApplePMGR) and by the
// real VerifiedRoot archive - not by loosening this file.

#pragma GCC visibility push(hidden)

#include <stddef.h>
#include <stdint.h>

extern "C" {
extern int kprintf(const char *format, ...);
extern void panic(const char *format, ...) __attribute__((noreturn));

// Tail-call target of every generated IOBlockStorageDevice thunk.  It cannot
// name the method (a thunk carries no metadata), so it names the consequence.
void hummer_storage_unavailable(void)
{
	kprintf("HUMMER STUB: an IOBlockStorageDevice base method was called; "
		"the storage class is not built into this probe image\n");
	panic("HUMMER STUB: IOBlockStorageDevice base method");
}

// ---- IOBlockStorageDevice vtable storage (the real class is a prelinked kext) ----
//
// 190 slots is the length our NativeUfs.o was compiled against; the array only
// has to exist so the vtable reference resolves.  It is zero, so an accidental
// virtual dispatch through it faults instead of running something.
extern void *hummer_storage_device_vtable[190] __asm__("__ZTV20IOBlockStorageDevice");
void *hummer_storage_device_vtable[190] = {};

// ---- VerifiedRoot: present, but refuses to authenticate anything ----
struct hummer_vr_crypto_stub {
	int (*sha256)(const void *, size_t, uint8_t[32]);
	int (*rsa3072_sha256)(const uint8_t[395], const uint8_t[32], const uint8_t[384]);
};
struct hummer_vr_view_stub;

extern const uint64_t verified_root_policy_count;
const uint64_t verified_root_policy_count = 0;
extern const unsigned char verified_root_policies[4096];
const unsigned char verified_root_policies[4096] = {0};
extern const unsigned char verified_root_key[395];
const unsigned char verified_root_key[395] = {0};
extern const struct hummer_vr_crypto_stub vr_xnu_crypto;
const struct hummer_vr_crypto_stub vr_xnu_crypto = {0, 0};

extern int vr_open(const uint8_t *, size_t, const uint8_t[395], uint64_t,
		   const struct hummer_vr_crypto_stub *, struct hummer_vr_view_stub *);
int vr_open(const uint8_t *, size_t, const uint8_t[395], uint64_t,
	    const struct hummer_vr_crypto_stub *, struct hummer_vr_view_stub *)
{
	kprintf("HUMMER STUB: vr_open reached; the owner root is not built into this probe image\n");
	panic("HUMMER STUB: vr_open");
}

extern int vr_verify_block(const struct hummer_vr_view_stub *, uint64_t, const void *, size_t,
			   const struct hummer_vr_crypto_stub *);
int vr_verify_block(const struct hummer_vr_view_stub *, uint64_t, const void *, size_t,
		    const struct hummer_vr_crypto_stub *)
{
	kprintf("HUMMER STUB: vr_verify_block reached; "
		"the owner root is not built into this probe image\n");
	panic("HUMMER STUB: vr_verify_block");
}

// NativeUfs declares this weak: absent in ordinary builds, a no-op by contract.
void boot_progress_poll(void)
{
}
}

#pragma GCC visibility pop

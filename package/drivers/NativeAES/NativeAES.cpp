// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalAESABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/crypto/aes.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#include <libkern/crypto/crypto_internal.h>
#pragma clang diagnostic pop
#include <corecrypto/cc.h>
#include <kern/task.h>

extern "C" {
volatile uint32_t native_aes_kat_status = 0;
volatile uint32_t native_aes_registered = 0;
volatile uint32_t native_aes_interface_status = 0;
volatile uint32_t native_aes_completed = 0;
volatile uint32_t native_aes_unsupported_key = 0;
}

class NativeAES : public IOAESAccelerator {
  OSDeclareDefaultStructors(NativeAES)
public:
  bool start(IOService *) override;
  IOWorkLoop *getWorkLoop() const override { return IOService::getWorkLoop(); }
  IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *a, void *b, void *c, void *d) override {
    return IOService::callPlatformFunction(name, wait, a, b, c, d);
  }
  const char *getAlgorithmName() override { return "AES-CBC"; }
  bool isKeySupported(unsigned selector) override { return selector == 0; }
  unsigned getSupportedOptions() override { return 1; }
  IOReturn performAES(void *, void *, task_t, unsigned long long, IOAESIV *,
                     IOAESOperation, IOAESKeyData *, IOAESCompletion, void *) override;
  IOReturn performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, unsigned long long,
                     IOAESIV *, IOAESOperation, IOAESKeyData *, unsigned long long,
                     unsigned long long, IOAESCompletion, void *) override;
};
OSDefineMetaClassAndStructors(NativeAES, IOAESAccelerator)

// No padding, hidden device keys, or change to the registered corecrypto modes.
static int cbc(const unsigned char *input, unsigned char *output, size_t bytes,
               unsigned char iv[16], const IOAESKeyData &key, IOAESOperation operation)
{
  union { aes_encrypt_ctx encrypt; aes_decrypt_ctx decrypt; } context;
  unsigned char nextIV[16];
  int result;
  if (operation == IOAESDecrypt) {
    memcpy(nextIV, input + bytes - 16, 16);
    result = aes_decrypt_key(key.bytes, key.bits / 8, &context.decrypt);
    if (!result) result = aes_decrypt_cbc(input, iv, static_cast<unsigned>(bytes / 16), output, &context.decrypt);
  } else {
    result = aes_encrypt_key(key.bytes, key.bits / 8, &context.encrypt);
    if (!result) result = aes_encrypt_cbc(input, iv, static_cast<unsigned>(bytes / 16), output, &context.encrypt);
    if (!result) memcpy(nextIV, output + bytes - 16, 16);
  }
  if (!result) memcpy(iv, nextIV, 16);
  cc_clear(sizeof(context), &context);
  cc_clear(sizeof(nextIV), nextIV);
  return result;
}

// NIST SP800-38A F.2.1/F.2.2, four CBC blocks, then split/in-place decryption.
static bool knownAnswerTest(NativeAES *provider)
{
  const unsigned char input[64] = {
    0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
    0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
    0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
    0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10};
  const unsigned char expected[64] = {
    0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d,
    0x50,0x86,0xcb,0x9b,0x50,0x72,0x19,0xee,0x95,0xdb,0x11,0x3a,0x91,0x76,0x78,0xb2,
    0x73,0xbe,0xd6,0xb8,0xe3,0xc1,0x74,0x3b,0x71,0x16,0xe6,0x9e,0x22,0x22,0x95,0x16,
    0x3f,0xf1,0xca,0xa1,0x68,0x1f,0xac,0x09,0x12,0x0e,0xca,0x30,0x75,0x86,0xe1,0xa7};
  IOAESKeyData key = {128, {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                           0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c}, 0};
  unsigned char iv[16], output[64];
  for (unsigned i = 0; i < 16; ++i) iv[i] = static_cast<unsigned char>(i);
  bool result = !cbc(input, output, 64, iv, key, IOAESEncrypt) && !memcmp(output, expected, 64) &&
                !memcmp(iv, expected + 48, 16);
  for (unsigned i = 0; i < 16; ++i) iv[i] = static_cast<unsigned char>(i);
  result = result && !cbc(output, output, 16, iv, key, IOAESDecrypt) &&
           !cbc(output + 16, output + 16, 48, iv, key, IOAESDecrypt) && !memcmp(output, input, 64) &&
           !memcmp(iv, expected + 48, 16);
  const IOAESKeyData wideKeys[2] = {
    {192, {0x8e,0x73,0xb0,0xf7,0xda,0x0e,0x64,0x52,0xc8,0x10,0xf3,0x2b,0x80,0x90,0x79,0xe5,
           0x62,0xf8,0xea,0xd2,0x52,0x2c,0x6b,0x7b}, 0},
    {256, {0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,
           0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4}, 0}};
  const unsigned char wideExpected[2][16] = {
    {0x4f,0x02,0x1d,0xb2,0x43,0xbc,0x63,0x3d,0x71,0x78,0x18,0x3a,0x9f,0xa0,0x71,0xe8},
    {0xf5,0x8c,0x4c,0x04,0xd6,0xe5,0xf1,0xba,0x77,0x9e,0xab,0xfb,0x5f,0x7b,0xfb,0xd6}};
  for (unsigned n = 0; n < 2; ++n) {
    for (unsigned i = 0; i < 16; ++i) iv[i] = static_cast<unsigned char>(i);
    result = result && !cbc(input, output, 16, iv, wideKeys[n], IOAESEncrypt) && !memcmp(output, wideExpected[n], 16);
    for (unsigned i = 0; i < 16; ++i) iv[i] = static_cast<unsigned char>(i);
    result = result && !cbc(output, output, 16, iv, wideKeys[n], IOAESDecrypt) && !memcmp(output, input, 16);
  }
  IOBufferMemoryDescriptor *source = IOBufferMemoryDescriptor::withCapacity(128, kIODirectionInOut);
  IOBufferMemoryDescriptor *destination = IOBufferMemoryDescriptor::withCapacity(128, kIODirectionInOut);
  IOAESIV publicIV;
  for (unsigned i = 0; i < 16; ++i) publicIV.bytes[i] = static_cast<unsigned char>(i);
  struct Completion { unsigned count; int status; } completion = {};
  auto callback = [](void *argument, int status) {
    Completion *state = static_cast<Completion *>(argument);
    state->count++;
    state->status = status;
  };
  bool interface = source && destination && source->writeBytes(16, input, 64) == 64 &&
    !provider->performAES(source, destination, 64, &publicIV, IOAESEncrypt, &key, 16, 32, callback, &completion) &&
    destination->readBytes(32, output, 64) == 64 && !memcmp(output, expected, 64) &&
    !memcmp(publicIV.bytes, expected + 48, 16) && completion.count == 1 && completion.status == 0;
  key.selector = 2001; // Original hardware-key selector, deliberately unsupported.
  interface = interface && provider->performAES(source, destination, 64, &publicIV,
    IOAESEncrypt, &key, 16, 32, callback, &completion) == kIOReturnUnsupported && completion.count == 1 &&
    destination->readBytes(32, output, 64) == 64 && !memcmp(output, expected, 64) &&
    !memcmp(publicIV.bytes, expected + 48, 16) && !provider->isKeySupported(key.selector);
  key.selector = 0;
  interface = interface && provider->performAES(source, destination, 64, &publicIV,
    IOAESEncrypt, &key, 16, 80, callback, &completion) == kIOReturnBadArgument && completion.count == 1;
  for (unsigned i = 0; i < 16; ++i) publicIV.bytes[i] = static_cast<unsigned char>(i);
  interface = interface && !provider->performAES(const_cast<unsigned char *>(input), output, kernel_task,
    64, &publicIV, IOAESEncrypt, &key, nullptr, nullptr) && !memcmp(output, expected, 64);
  for (unsigned i = 0; i < 16; ++i) publicIV.bytes[i] = static_cast<unsigned char>(i);
  interface = interface && !provider->performAES(output, output, kernel_task, 64, &publicIV,
    IOAESDecrypt, &key, nullptr, nullptr) && !memcmp(output, input, 64) && !memcmp(publicIV.bytes, expected + 48, 16);
  native_aes_interface_status = interface ? 1 : 2;
  if (source) source->release();
  if (destination) destination->release();
  result = result && interface;
  cc_clear(sizeof(key), &key);
  cc_clear(sizeof(output), output);
  return result;
}

bool NativeAES::start(IOService *provider)
{
  if (!g_crypto_funcs || !g_crypto_funcs->ccaes_cbc_encrypt || !g_crypto_funcs->ccaes_cbc_decrypt ||
      !IOService::start(provider)) return false;
  native_aes_kat_status = knownAnswerTest(this) ? 1 : 2;
  if (native_aes_kat_status != 1) return false;
  setProperty("NativeAESSoftwareKeysOnly", true);
  native_aes_registered = 1;
  registerService();
  return true;
}

IOReturn NativeAES::performAES(IOMemoryDescriptor *input, IOMemoryDescriptor *output,
                              unsigned long long bytes, IOAESIV *iv, IOAESOperation operation,
                              IOAESKeyData *key, unsigned long long inputOffset,
                              unsigned long long outputOffset, IOAESCompletion callback, void *refcon)
{
  if (!input || !output || !key || !iv || !bytes || (bytes & 15) ||
      (key->bits != 128 && key->bits != 192 && key->bits != 256)) return kIOReturnBadArgument;
  if (key->selector || (operation != IOAESEncrypt && operation != IOAESDecrypt)) {
    __atomic_fetch_add(&native_aes_unsupported_key, 1, __ATOMIC_RELAXED);
    return kIOReturnUnsupported;
  }
  if (inputOffset > input->getLength() || bytes > input->getLength() - inputOffset ||
      outputOffset > output->getLength() || bytes > output->getLength() - outputOffset) return kIOReturnBadArgument;
  unsigned char *buffer = static_cast<unsigned char *>(IOMalloc(4096));
  if (!buffer) return kIOReturnNoMemory;
  IOReturn result = input->prepare();
  if (result) {
    IOFree(buffer, 4096);
    return result;
  }
  result = output->prepare();
  if (result) {
    input->complete();
    IOFree(buffer, 4096);
    return result;
  }
  unsigned char chaining[16];
  memcpy(chaining, iv->bytes, 16);
  for (unsigned long long done = 0; done < bytes; ) {
    size_t count = static_cast<size_t>(bytes - done > 4096 ? 4096 : bytes - done);
    if (input->readBytes(inputOffset + done, buffer, count) != count ||
        cbc(buffer, buffer, count, chaining, *key, operation) ||
        output->writeBytes(outputOffset + done, buffer, count) != count) {
      result = kIOReturnIOError;
      break;
    }
    done += count;
  }
  if (!result) {
    memcpy(iv->bytes, chaining, 16);
    __atomic_fetch_add(&native_aes_completed, 1, __ATOMIC_RELAXED);
  }
  cc_clear(4096, buffer);
  IOFree(buffer, 4096);
  cc_clear(sizeof(chaining), chaining);
  output->complete();
  input->complete();
  if (callback) {
    callback(refcon, result);
    return kIOReturnSuccess;
  }
  return result;
}

IOReturn NativeAES::performAES(void *input, void *output, task_t task, unsigned long long bytes,
                              IOAESIV *iv, IOAESOperation operation, IOAESKeyData *key,
                              IOAESCompletion callback, void *refcon)
{
  if (!task || !input || !output || !bytes) return kIOReturnBadArgument;
  IOMemoryDescriptor *source = IOMemoryDescriptor::withAddressRange(
      reinterpret_cast<mach_vm_address_t>(input), bytes, kIODirectionOut, task);
  IOMemoryDescriptor *destination = IOMemoryDescriptor::withAddressRange(
      reinterpret_cast<mach_vm_address_t>(output), bytes, kIODirectionIn, task);
  if (!source || !destination) {
    if (source) source->release();
    if (destination) destination->release();
    return kIOReturnNoMemory;
  }
  IOReturn result = performAES(source, destination, bytes, iv, operation, key, 0, 0, callback, refcon);
  source->release();
  destination->release();
  return result;
}

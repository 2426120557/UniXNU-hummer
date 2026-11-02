#!/usr/bin/env python3
"""Compile the real loader ADT parser and check the DRAM placeholder contract."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


def node(name, properties=(), children=()):
    props = [('name', name.encode() + b'\0'), *properties]
    data = struct.pack('<II', len(props), len(children))
    for key, value in props:
        data += struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return data + b''.join(children)


class AdtParserTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(__file__).resolve().parents[1] / 'DarwinBootPkg/Driver'
        source = (root / 'DarwinBoot.c').read_text()
        body = source[source.index('STATIC\nBOOLEAN\nAdtNode ('):source.index('STATIC\nBOOLEAN\nValidTrustCaches (')]
        code = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "WindowSize.h"
#define STATIC static
#define CONST const
#define TRUE 1
#define FALSE 0
#define MAX_UINTN SIZE_MAX
#define ALIGN_VALUE(v,a) (((v)+(a)-1)&~((a)-1))
#define ScanMem8 memchr
#define AsciiStrCmp strcmp
#define CompareMem memcmp
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef char CHAR8;
typedef int BOOLEAN;
static uint32_t U32(const void *p) { uint32_t n; memcpy(&n,p,4); return n; }
static uint64_t U64(const void *p) { uint64_t n; memcpy(&n,p,8); return n; }
'''
        # UEFI ScanMem8 has (buffer, length, value), unlike memchr.
        code = code.replace('#define ScanMem8 memchr', '#define ScanMem8(p,n,v) memchr(p,v,n)')
        code += body + r'''
int parse(const uint8_t *data, size_t size, size_t out[2]) {
    size_t offset=0, ram=SIZE_MAX, trust=SIZE_MAX;
    uint64_t window=0; int packed=0;
    out[0]=out[1]=SIZE_MAX;
    return AdtNode(data,size,&offset,0,3,&ram,&trust,&window,&packed,out) && offset==size;
}
'''
        path = Path(cls.temp.name)
        (path / 'parser.c').write_text(code)
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', '-I', root,
                        path / 'parser.c', '-o', path / 'parser.so'], check=True)
        cls.lib = ctypes.CDLL(str(path / 'parser.so'))
        cls.lib.parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

    def parse(self, data):
        offsets = (ctypes.c_size_t * 2)()
        return self.lib.parse(data, len(data), offsets), list(offsets)

    def test_placeholder_offsets(self):
        data = node('device-tree', children=[node('chosen', [
            ('darwinboot-memory-size', struct.pack('<Q', 0x10000000)),
            ('dram-base', bytes(8)), ('dram-size', bytes(8))])])
        valid, offsets = self.parse(data)
        self.assertEqual(valid, 1)
        self.assertEqual([data[o:o + 8] for o in offsets], [bytes(8), bytes(8)])
        self.assertNotEqual(*offsets)
        for length in range(len(data)):
            self.assertEqual(self.parse(data[:length])[0], 0)

    def test_malformed_dram_fields(self):
        for properties, name in [([('dram-base', bytes(4))], 'chosen'),
                                 ([('dram-base', bytes(8)), ('dram-base', bytes(8))], 'chosen'),
                                 ([('dram-size', bytes(8))], 'options')]:
            self.assertEqual(self.parse(node('device-tree', children=[node(name, properties)]))[0], 0)
        chosen = node('chosen', [('dram-base', bytes(8)), ('dram-size', bytes(8))])
        self.assertEqual(self.parse(node('device-tree', children=[chosen, chosen]))[0], 0)


if __name__ == '__main__':
    unittest.main()

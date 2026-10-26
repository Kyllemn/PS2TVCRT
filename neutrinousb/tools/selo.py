#!/usr/bin/env python3
"""selo.py - grava o selo de integridade no ELF (NEUTRINOUSB).

CRC32 de .text + .rodata (o mesmo que seal_ok() calcula na memoria) vai para
rv_seal[2], e o tamanho para rv_seal[3]. A marca "RVLT" "LSAL" fica no .data.
Uso: selo.py <elf>  (altera o arquivo)
"""
import struct, sys, zlib

def sections(b):
    shoff = struct.unpack_from('<I', b, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x2E)
    hdrs = []
    for i in range(shnum):
        h = struct.unpack_from('<IIIIIIIIII', b, shoff + i * shentsize)
        hdrs.append(h)
    stro = hdrs[shstrndx][4]
    out = {}
    for h in hdrs:
        name = b[stro + h[0]:b.index(b'\0', stro + h[0])].decode()
        out[name] = h  # name, type, flags, addr, offset, size, ...
    return out

def main(path):
    b = bytearray(open(path, 'rb').read())
    s = sections(b)
    t, r, d = s['.text'], s['.rodata'], s['.data']
    text = bytes(b[t[4]:t[4] + t[5]])
    ro = bytes(b[r[4]:r[4] + r[5]])
    crc = zlib.crc32(ro, zlib.crc32(text)) & 0xFFFFFFFF
    marca = struct.pack('<II', 0x544C5652, 0x4C41534C)
    i = b.find(marca, d[4], d[4] + d[5])
    if i < 0:
        sys.exit('selo: marca nao encontrada no .data')
    struct.pack_into('<II', b, i + 8, crc, len(text) + len(ro))
    open(path, 'wb').write(b)
    print('selo: crc %08X, %d bytes (.text %d + .rodata %d)' % (crc, len(text) + len(ro), len(text), len(ro)))

main(sys.argv[1])

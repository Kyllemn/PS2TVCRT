"""ELF (mipsel) -> PS-EXE. Usa so os segmentos LOAD (o elf2x do SDK se perde com o GNU_STACK
do toolchain mipsel-linux-gnu)."""
import sys, struct, subprocess
elf, out = sys.argv[1], sys.argv[2]
d = open(elf, 'rb').read()
e_entry = struct.unpack_from('<I', d, 24)[0]
phoff, = struct.unpack_from('<I', d, 28); phentsize, phnum = struct.unpack_from('<HH', d, 42)
segs = []
for i in range(phnum):
    t, off, va, pa, fsz, msz = struct.unpack_from('<6I', d, phoff + i*phentsize)
    if t == 1 and fsz: segs.append((va, d[off:off+fsz], msz))
assert len(segs) == 1, segs
va, data, msz = segs[0]
data += b'\0' * (-len(data) % 2048)
syms = subprocess.run(['mipsel-linux-gnu-nm', elf], capture_output=True, text=True).stdout
gp = next((int(l.split()[0], 16) for l in syms.splitlines() if l.endswith(' _gp')), 0)
bss = va + len(data); bss_sz = max(0, va + msz - bss)
h = bytearray(2048)
h[0:8] = b'PS-X EXE'
struct.pack_into('<10I', h, 0x10, e_entry, gp, va, len(data), 0, 0, bss, bss_sz, 0x801FFFF0, 0)
h[0x4c:0x4c+len(b'Sony Computer Entertainment Inc. for Japan area')] = b'Sony Computer Entertainment Inc. for Japan area'
open(out, 'wb').write(bytes(h) + data)
print(out, 'pc=%08x gp=%08x t=%08x+%x bss=%08x+%x' % (e_entry, gp, va, len(data), bss, bss_sz))

#!/usr/bin/env python3
# mkbin.py ENTRADA SAIDA.c NOME  ->  const unsigned char NOME[]; const unsigned int NOME_size;
import sys
data = open(sys.argv[1], 'rb').read()
name = sys.argv[3]
with open(sys.argv[2], 'w') as f:
    f.write('const unsigned int %s_size = %d;\n' % (name, len(data)))
    f.write('const unsigned char %s[] __attribute__((aligned(16))) = {\n' % name)
    for i in range(0, len(data), 16):
        f.write(''.join('0x%02x,' % b for b in data[i:i + 16]) + '\n')
    f.write('};\n')

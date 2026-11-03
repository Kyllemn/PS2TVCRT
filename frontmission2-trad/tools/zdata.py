"""ZDATA.BIN: u32 n, u32 setores_de_cabecalho, depois n pares (setor, tamanho em bytes); subarquivos alinhados a 2048"""
import struct
def entries(z):
    n=struct.unpack('<I',z[:4])[0]
    return [struct.unpack('<2I',z[8+8*i:16+8*i]) for i in range(n)]
def sub(z,i):
    s,sz=entries(z)[i]; return z[s*2048:s*2048+sz]

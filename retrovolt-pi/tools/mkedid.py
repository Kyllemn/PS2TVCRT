"""Gera a EDID do RetroVolt para TV de tubo (15 kHz) via HDMI > VGA > componente.

O HDMI do Pi 5 nao gera pixel clock abaixo de 25 MHz, entao os modos de TV usam
"super-resolucao": 1920 pontos por linha com a mesma frequencia horizontal da TV
(~15,7 kHz). O adaptador converte e a TV recebe 240p/288p/480i normais.

uso: mkedid.py saida.bin
"""
import struct
import sys


def dtd(pclk_hz, ha, hfp, hs, hbp, va, vfp, vs, vbp, interlaced=False, w_mm=300, h_mm=225):
    """Detailed Timing Descriptor (18 bytes). Valores verticais por campo se entrelacado."""
    hb = hfp + hs + hbp
    vb = vfp + vs + vbp
    p = round(pclk_hz / 10000)
    d = bytearray(18)
    struct.pack_into('<H', d, 0, p)
    d[2] = ha & 0xff
    d[3] = hb & 0xff
    d[4] = ((ha >> 8) << 4) | (hb >> 8)
    d[5] = va & 0xff
    d[6] = vb & 0xff
    d[7] = ((va >> 8) << 4) | (vb >> 8)
    d[8] = hfp & 0xff
    d[9] = hs & 0xff
    d[10] = ((vfp & 15) << 4) | (vs & 15)
    d[11] = ((hfp >> 8) << 6) | ((hs >> 8) << 4) | ((vfp >> 4) << 2) | (vs >> 4)
    d[12] = w_mm & 0xff
    d[13] = h_mm & 0xff
    d[14] = ((w_mm >> 8) << 4) | (h_mm >> 8)
    # sync separado digital, ambos negativos (bits 4-3 = 11, bit2 vsync -, bit1 hsync -)
    d[17] = (0x80 if interlaced else 0) | 0x18
    return bytes(d)


def name_desc(text):
    t = text.encode('ascii')[:13]
    t = t + (b'\n' + b' ' * 12)[:13 - len(t)] if len(t) < 13 else t
    return bytes([0, 0, 0, 0xFC, 0]) + t


def edid():
    e = bytearray(128)
    e[0:8] = b'\x00\xff\xff\xff\xff\xff\xff\x00'
    # fabricante "RTV" (letras A=1): R=18 T=20 V=22
    m = (18 << 10) | (20 << 5) | 22
    e[8:10] = struct.pack('>H', m)
    e[10:12] = struct.pack('<H', 0x0240)      # produto
    e[12:16] = struct.pack('<I', 1)           # serial
    e[16] = 45; e[17] = 2026 - 1990           # semana / ano
    e[18] = 1; e[19] = 3                      # EDID 1.3
    e[20] = 0x80                              # entrada digital
    e[21] = 30; e[22] = 23                    # 30x23 cm (4:3)
    e[23] = 120                               # gamma 2.2
    e[24] = 0x0A                              # RGB, modo preferido = 1o DTD
    # cromaticidade padrao sRGB
    e[25:35] = bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])
    e[35:38] = b'\x00\x00\x00'                # sem modos padrao VESA
    e[38:54] = b'\x01\x01' * 8               # sem timings padrao
    # NTSC 240p: 2440 x 262 linhas, 60.0 Hz -> 15,72 kHz
    d1 = dtd(2440 * 262 * 60.0, 1920, 80, 180, 260, 240, 3, 3, 16)
    # PAL 288p: 2456 x 312 linhas, 50 Hz -> 15,6 kHz
    d2 = dtd(2456 * 312 * 50.0, 1920, 80, 184, 272, 288, 3, 3, 18)
    # NTSC 480i: por campo 240 linhas + 22,5 de intervalo (525 por quadro)
    d3 = dtd(2440 * 525 * 30.0, 1920, 80, 180, 260, 240, 3, 3, 16, interlaced=True)
    e[54:72] = d1
    e[72:90] = d2
    e[90:108] = d3
    e[108:126] = name_desc('RETROVOLT CRT')
    e[126] = 1                                 # 1 extensao (CEA-861: som pelo HDMI)
    e[127] = (-sum(e[:127])) & 0xff
    return bytes(e) + cea()


def cea():
    """Extensao CEA-861: diz ao Pi que a "TV" aceita som (PCM 2 canais) e e HDMI.
    Sem isso o Pi manda video sem som (modo DVI). Nao lista modos de video."""
    blocos = bytes([
        0x23, 0x09, 0x07, 0x07,                # audio: LPCM 2 canais, 32/44,1/48 kHz, 16/20/24 bits
        0x83, 0x01, 0x00, 0x00,                # alto-falantes: esquerdo + direito
        0x65, 0x03, 0x0C, 0x00, 0x10, 0x00,    # HDMI (OUI 00-0C-03), endereco 1.0.0.0
    ])
    c = bytearray(128)
    c[0] = 0x02                                # CEA
    c[1] = 0x03                                # revisao 3
    c[2] = 4 + len(blocos)                     # inicio dos DTDs (nenhum)
    c[3] = 0x40                                # suporta som basico
    c[4:4 + len(blocos)] = blocos
    c[127] = (-sum(c[:127])) & 0xff
    return bytes(c)


if __name__ == '__main__':
    b = edid()
    open(sys.argv[1], 'wb').write(b)
    print('EDID', len(b), 'bytes, checksum ok' if sum(b[:128]) % 256 == 0 and sum(b[128:]) % 256 == 0 else 'ERRO')

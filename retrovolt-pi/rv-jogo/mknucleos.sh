#!/bin/bash
# compila os nucleos para o Pi 5 (aarch64) e copia para versoes/rvNN/nucleos/
# uso: mknucleos.sh pasta_da_versao   (precisa de gcc-aarch64-linux-gnu)
set -e
DEST="$1/nucleos"; mkdir -p "$DEST"
T=$(mktemp -d)
git clone -q --depth 1 https://github.com/libretro/pcsx_rearmed "$T/pcsx"
( cd "$T/pcsx" && git submodule update --init --depth 1 -q &&
  make -f Makefile.libretro platform=rpi4_64 CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar \
       HAVE_PHYSICAL_CDROM=0 -j"$(nproc)" >/dev/null )   # leitora: pelo CD virtual do rv-jogo
aarch64-linux-gnu-strip -o "$DEST/pcsx_rearmed_libretro.so" "$T/pcsx/pcsx_rearmed_libretro.so"
echo "pcsx_rearmed $(cd "$T/pcsx" && git log -1 --format=%h)"
git clone -q --depth 1 https://github.com/libretro/beetle-pce-fast-libretro "$T/pce"
( cd "$T/pce" && make platform=unix CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ AR=aarch64-linux-gnu-ar \
       HAVE_CDROM=0 -j"$(nproc)" >/dev/null )   # PC Engine e PC Engine CD (leitora pelo CD virtual)
aarch64-linux-gnu-strip -o "$DEST/mednafen_pce_fast_libretro.so" "$T/pce/mednafen_pce_fast_libretro.so"
echo "beetle-pce-fast $(cd "$T/pce" && git log -1 --format=%h)"
rm -rf "$T"

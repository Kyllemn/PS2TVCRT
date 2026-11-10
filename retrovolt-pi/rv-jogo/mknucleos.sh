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
git clone -q --depth 1 https://github.com/libretro/Genesis-Plus-GX "$T/gpgx"
( cd "$T/gpgx" && make -f Makefile.libretro platform=unix CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )   # Sega CD, Mega Drive, Master System (CD pelo VFS)
aarch64-linux-gnu-strip -o "$DEST/genesis_plus_gx_libretro.so" "$T/gpgx/genesis_plus_gx_libretro.so"
echo "genesis-plus-gx $(cd "$T/gpgx" && git log -1 --format=%h)"
git clone -q --depth 1 https://github.com/libretro/neocd_libretro "$T/neocd"
python3 "$(dirname "$0")/remendos/neocd_nomes_sem_caixa.py" "$T/neocd"
( cd "$T/neocd" && make platform=unix CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )   # Neo Geo CD (CD pelo VFS; BIOS opcional: tem HLE)
aarch64-linux-gnu-strip -o "$DEST/neocd_libretro.so" "$T/neocd/neocd_libretro.so"
echo "neocd $(cd "$T/neocd" && git log -1 --format=%h)"
git clone -q --depth 1 https://github.com/libretro/picodrive "$T/pico"
( cd "$T/pico" && git submodule update --init --depth 1 -q &&
  make -f Makefile.libretro platform=aarch64 CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )   # 32X (e MD/SMS/Sega CD, mas esses ficam no GPGX)
aarch64-linux-gnu-strip -o "$DEST/picodrive_libretro.so" "$T/pico/picodrive_libretro.so"
echo "picodrive $(cd "$T/pico" && git log -1 --format=%h)"
git clone -q --depth 1 https://github.com/libretro/snes9x "$T/snes9x"
( cd "$T/snes9x/libretro" && make platform=unix CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )   # Super Nintendo
aarch64-linux-gnu-strip -o "$DEST/snes9x_libretro.so" "$T/snes9x/libretro/snes9x_libretro.so"
echo "snes9x $(cd "$T/snes9x" && git log -1 --format=%h)"
# Dreamcast: Flycast com GLES 3 (a GPU V3D do Pi 5) e JIT aarch64. Precisa das bibliotecas
# GLES do arm64 para ligar (dpkg --add-architecture arm64; apt-get install libgles-dev:arm64).
# PAGE_SIZE 16384: o kernel do Pi 5 usa paginas de 16 KB; com o padrao (4 KB) o mprotect
# da memoria emulada falha e o Flycast para (SIGTRAP, "codigo 5"). 16 KB vale tambem em 4 KB.
git clone -q --depth 1 https://github.com/flyinghead/flycast "$T/flycast"
( cd "$T/flycast" && git submodule update --init --recursive --depth 1 -q &&
  cat > "$T/aarch64.cmake" <<'TC'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
TC
  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$T/aarch64.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DLIBRETRO=ON -DUSE_GLES=ON -DUSE_OPENGL=ON -DUSE_VULKAN=OFF -DUSE_LUA=OFF -DUSE_BREAKPAD=OFF \
        -DCMAKE_C_FLAGS="-mcpu=cortex-a76 -DPAGE_SIZE=16384" \
        -DCMAKE_CXX_FLAGS="-mcpu=cortex-a76 -DPAGE_SIZE=16384" >/dev/null &&
  cmake --build build -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/flycast_libretro.so" "$T/flycast/build/flycast_libretro.so"
echo "flycast $(cd "$T/flycast" && git log -1 --format=%h)"
rm -rf "$T"

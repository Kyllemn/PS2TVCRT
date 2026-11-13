#!/bin/bash
# compila os nucleos para o Pi 5 (aarch64) e copia para versoes/rvNN/nucleos/
# uso: mknucleos.sh pasta_da_versao   (precisa de gcc-aarch64-linux-gnu)
set -e
AQUI="$(cd "$(dirname "$0")" && pwd)"
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
python3 "$AQUI/remendos/neocd_nomes_sem_caixa.py" "$T/neocd"
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
( cd "$T/snes9x/libretro" && make platform=unix CC="aarch64-linux-gnu-gcc -march=armv8.2-a+crc+simd -mtune=cortex-a76" \
       CXX="aarch64-linux-gnu-g++ -march=armv8.2-a+crc+simd -mtune=cortex-a76" \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )   # Super Nintendo
aarch64-linux-gnu-strip -o "$DEST/snes9x_libretro.so" "$T/snes9x/libretro/snes9x_libretro.so"
echo "snes9x $(cd "$T/snes9x" && git log -1 --format=%h)"
# Dreamcast: Flycast com Vulkan (padrao; o GLES do Pi pinta 2 vezes o canto de blocos 2D) e
# GLES 3 de reserva, JIT aarch64. No Pi: mesa-vulkan-drivers + libvulkan1. Precisa das bibliotecas
# GLES do arm64 para ligar (dpkg --add-architecture arm64; apt-get install libgles-dev:arm64).
# PAGE_SIZE 16384: o kernel do Pi 5 usa paginas de 16 KB; com o padrao (4 KB) o mprotect
# da memoria emulada falha e o Flycast para (SIGTRAP, "codigo 5"). 16 KB vale tambem em 4 KB.
git clone -q --depth 1 https://github.com/flyinghead/flycast "$T/flycast"
( cd "$T/flycast" && git submodule update --init --recursive --depth 1 -q &&
  python3 "$AQUI/remendos/flycast_page_size.py" . &&
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
        -DLIBRETRO=ON -DUSE_GLES=ON -DUSE_OPENGL=ON -DUSE_VULKAN=ON -DUSE_LUA=OFF -DUSE_BREAKPAD=OFF \
        -DPAGE_SIZE=16384 -DCMAKE_C_FLAGS="-mcpu=cortex-a76" -DCMAKE_CXX_FLAGS="-mcpu=cortex-a76" >/dev/null &&
  cmake --build build -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/flycast_libretro.so" "$T/flycast/build/flycast_libretro.so"
echo "flycast $(cd "$T/flycast" && git log -1 --format=%h)"
# Saturno (principal): Beetle Saturn v1.32 (o do ReplayOS): desenho por CPU, JIT aarch64 nos
# DSPs (SCU e SCSP), memoria do JIT alinhada em 64 KB (vale com as paginas de 16 KB do Pi 5).
# VFS_HYBRID_SANDBOXED=1: o VFS "hibrido" dele so passa ao frontend caminhos "xxx://"; o
# disco.cue existe de verdade e abre, mas o disco.bin so existe no CD virtual ("Failed to open
# CD"). Assim, o que falhar no arquivo real vai para o rv-jogo.
git clone -q --depth 1 https://github.com/libretro/beetle-saturn-libretro "$T/bsat"
( cd "$T/bsat" && make platform=unix CC="aarch64-linux-gnu-gcc -mcpu=cortex-a76 -DVFS_HYBRID_SANDBOXED=1" \
       CXX="aarch64-linux-gnu-g++ -mcpu=cortex-a76 -DVFS_HYBRID_SANDBOXED=1" \
       AR=aarch64-linux-gnu-ar HAVE_CDROM=0 WANT_JIT=1 SILENT=1 -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/mednafen_saturn_libretro.so" "$T/bsat/mednafen_saturn_libretro.so"
echo "beetle-saturn $(cd "$T/bsat" && git log -1 --format=%h)"
# Arcade: FBNeo (CPS1/2/3, Neo Geo MVS/AES...). platform=rpi5_64 = cortex-a76, -O3. Sem LTO/
# flags experimentais (quebram jogos segundo o proprio FBNeo)
git clone -q --depth 1 https://github.com/libretro/FBNeo "$T/fbneo"
( cd "$T/fbneo/src/burner/libretro" && make platform=rpi5_64 CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/fbneo_libretro.so" "$T/fbneo/src/burner/libretro/fbneo_libretro.so"
echo "fbneo $(cd "$T/fbneo" && git log -1 --format=%h)"
# NES/Famicom Disk System: Mesen (preciso; disksys.rom opcional para .fds)
F="-march=armv8.2-a+crc+simd -mtune=cortex-a76"
git clone -q --depth 1 https://github.com/libretro/Mesen "$T/mesen"
( cd "$T/mesen/Libretro" && make platform=unix CC="aarch64-linux-gnu-gcc $F" CXX="aarch64-linux-gnu-g++ $F" \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/mesen_libretro.so" "$T/mesen/Libretro/mesen_libretro.so"
echo "mesen $(cd "$T/mesen" && git log -1 --format=%h)"
# Game Boy / Color: Gambatte
git clone -q --depth 1 https://github.com/libretro/gambatte-libretro "$T/gamb"
( cd "$T/gamb" && make -f Makefile.libretro platform=unix CC="aarch64-linux-gnu-gcc $F" CXX="aarch64-linux-gnu-g++ $F" \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/gambatte_libretro.so" "$T/gamb/gambatte_libretro.so"
echo "gambatte $(cd "$T/gamb" && git log -1 --format=%h)"
# Neo Geo Pocket / Color: Beetle NeoPop
git clone -q --depth 1 https://github.com/libretro/beetle-ngp-libretro "$T/ngp"
( cd "$T/ngp" && make platform=unix CC="aarch64-linux-gnu-gcc $F" CXX="aarch64-linux-gnu-g++ $F" \
       AR=aarch64-linux-gnu-ar -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/mednafen_ngp_libretro.so" "$T/ngp/mednafen_ngp_libretro.so"
echo "beetle-ngp $(cd "$T/ngp" && git log -1 --format=%h)"
# GBA: mGBA (CMake; so o nucleo libretro)
git clone -q --depth 1 https://github.com/libretro/mgba "$T/mgba"
( cd "$T/mgba" && cmake -B build -DCMAKE_TOOLCHAIN_FILE="$AQUI/aarch64.cmake" -DCMAKE_BUILD_TYPE=Release \
       -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON >/dev/null && cmake --build build --target mgba_libretro -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/mgba_libretro.so" "$T/mgba/build/mgba_libretro.so"
echo "mgba $(cd "$T/mgba" && git log -1 --format=%h)"
# DS: melonDS DS (CMake; baixa as dependencias). Desenho por CPU (OpenGL do Pi nao serve: ele
# pede OpenGL de PC; issue #262 do melonDS DS), JIT aarch64. ENABLE_NETWORKING=OFF nao compila
# (falta Net.h): fica com rede, desligada nas opcoes
git clone -q --depth 1 https://github.com/JesseTG/melonds-ds "$T/mds"
( cd "$T/mds" && cmake -B build -DCMAKE_TOOLCHAIN_FILE="$AQUI/aarch64.cmake" -DCMAKE_BUILD_TYPE=Release \
       -DENABLE_OPENGL=OFF -DENABLE_JIT=ON -DBUILD_TESTING=OFF >/dev/null && cmake --build build -j"$(nproc)" >/dev/null )
aarch64-linux-gnu-strip -o "$DEST/melondsds_libretro.so" "$T/mds/build/src/libretro/melondsds_libretro.so"
echo "melonds-ds $(cd "$T/mds" && git log -1 --format=%h)"
rm -rf "$T"

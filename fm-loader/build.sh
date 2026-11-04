#!/bin/sh
# compila o menu (PSn00bSDK + mipsel-linux-gnu) e gera build/FMMENU.EXE
set -e
cd "$(dirname "$0")"
cmake -S . -B cbuild -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=/opt/psx/sdk/lib/libpsn00b/cmake/sdk.cmake \
      -DPSN00BSDK_TARGET=mipsel-linux-gnu -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build cbuild
mkdir -p build
python3 tools/elf2psx.py cbuild/fmmenu.elf build/FMMENU.EXE
# MENU.DAT: python3 tools/menu_dat.py config.json saida/MENU.DAT (lista de jogos, capas, discos, fundo)

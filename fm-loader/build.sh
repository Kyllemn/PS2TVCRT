#!/bin/sh
# compila o menu (PSn00bSDK + mipsel-linux-gnu) e gera build/FMMENU.EXE e build/MENU.DAT
set -e
cd "$(dirname "$0")"
cmake -S . -B cbuild -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=/opt/psx/sdk/lib/libpsn00b/cmake/sdk.cmake \
      -DPSN00BSDK_TARGET=mipsel-linux-gnu -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build cbuild
mkdir -p build
python3 tools/elf2psx.py cbuild/fmmenu.elf build/FMMENU.EXE
python3 tools/assets.py build/MENU.DAT

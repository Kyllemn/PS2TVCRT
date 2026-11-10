#!/bin/bash
# compila o rv-jogo para o Pi 5 (aarch64) e para o PC (teste)
cd "$(dirname "$0")"
# cabecalhos do Vulkan (so os .h; a libvulkan e carregada na hora no Pi): baixados na 1a vez
VKH=terceiros/Vulkan-Headers
[ -f $VKH/include/vulkan/vulkan.h ] || git clone -q --depth 1 -b v1.3.296 https://github.com/KhronosGroup/Vulkan-Headers $VKH
FL="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -I$VKH/include"
aarch64-linux-gnu-gcc $FL -mcpu=cortex-a76 -o rv-jogo.arm64 rv-jogo.c -ldl -lpthread -lm && aarch64-linux-gnu-strip rv-jogo.arm64
gcc $FL -g -o rv-jogo.x86 rv-jogo.c -ldl -lpthread -lm

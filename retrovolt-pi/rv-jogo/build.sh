#!/bin/bash
# compila o rv-jogo para o Pi 5 (aarch64) e para o PC (teste)
cd "$(dirname "$0")"
FL="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
aarch64-linux-gnu-gcc $FL -mcpu=cortex-a76 -o rv-jogo.arm64 rv-jogo.c -ldl -lpthread -lm && aarch64-linux-gnu-strip rv-jogo.arm64
gcc $FL -g -o rv-jogo.x86 rv-jogo.c -ldl -lpthread -lm

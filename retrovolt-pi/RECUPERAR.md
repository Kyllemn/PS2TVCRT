# RETROVOLT Pi — como retomar o projeto numa sessao nova

Atualizado em 2026-11-13 (release R25, commit seguinte ao aa77150).

## 1. Prompt para colar na sessao nova (copie tudo dentro do bloco)

```
Voce vai continuar o projeto RETROVOLT Pi (frontend de emulacao para Raspberry Pi 5),
que eu (autor) venho fazendo com voce desde 28/10/2026. A sessao anterior caiu.

1) Pegue o codigo: repositorio kyllemn/ps2tvcrt, branch claude/new-session-508stw
   (git fetch origin claude/new-session-508stw && git checkout claude/new-session-508stw).
   Se eu te mandar o zip de backup, ele tem repositorio.bundle (git clone repositorio.bundle)
   e o ultimo pacote pronto (RETROVOLT_R25.zip).
2) Leia nesta ordem, antes de qualquer coisa: retrovolt-pi/RECUPERAR.md (este arquivo),
   retrovolt-pi/DESIGN.md (todas as decisoes e o historico R00..R25, o fim e o mais recente),
   retrovolt-pi/PLANO.md, retrovolt-pi/CONFIG.md.
3) Siga as REGRAS DO AUTOR da secao 2 do RECUPERAR.md sem excecao. A principal: NUNCA
   compilar nada (build.sh, mknucleos.sh, gcc, cmake, make de nucleo) sem eu mandar #compilar.
4) Depois de ler, me responda em pt-BR, curto: em que release estamos, o que ficou pendente
   e o que voce sugere fazer agora. Nao mude nada antes da minha resposta.
```

## 2. Regras do autor (valem sempre)
- Responder em pt-BR, curto e direto. Espaco/tamanho so em MB.
- **#compilar**: so compila (build.sh, mknucleos.sh, gcc, cmake, qualquer build de nucleo ou do
  rv-jogo) quando o autor mandar "#compilar". Comando absoluto. Script Python nao e compilacao.
- "nao faca nada" / "so me explique": so responder, sem mexer em arquivo.
- Arquivos de jogo e texto extraido de jogo NUNCA vao para o git. Nunca publicar logs.
- Nao baixar BIOS. Nada de XDK vazado nem site pirata (inclusive listas de IPTV de agregador).
- Python sempre com `python3 -B`.
- Git: commit e push so na branch `claude/new-session-508stw`. No fim da mensagem de commit:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` e a linha `Claude-Session:` da
  sessao atual. Nunca por id de modelo em commit/arquivo.
- Nunca repetir nem guardar a senha do Wi-Fi, o e-mail do autor, credenciais de IPTV (a lista
  adultos.m3u do autor tem usuario/senha do provedor), senha/PIN/NFAuthentication.key da
  Netflix, chaves do Google (AIza...) ou OAuth. Ficam so no retrovolt.cfg/pendrive do autor.

## 3. O que e o projeto
Raspberry Pi 5 (2 GB) que liga direto num menu proprio (tema "Editorial", 320x240, fonte 8x8)
na TV de tubo (240p pelo HDMI + conversor componente) ou num LCD. Leitora USB de CD/DVD
(roda os discos originais/gravados), caixa RetroFlag 64Pi (POWER/RESET), controle 8BitDo.
Jogos, BIOS, saves e config no pendrive vfat `/media/rvusb/RETROVOLT` (plano futuro: tudo no
cartao; o codigo usa uma pasta base so, `self.base`, e da para trocar).

## 4. Mapa do codigo (tudo em retrovolt-pi/)
- `r00/RETROVOLT/` = o que vai na raiz do pendrive: instalar.sh (instala no Pi OS Lite 64),
  atualizar.sh, caixa.sh, desfazer.sh, consertar_cartao.sh, sistema/, tv/, LEIA-ME*.
- `r00/RETROVOLT/versoes/rvNN/` = cada versao do menu. A atual e **rv23** (as releases R23..R25
  continuam na pasta rv23). Arquivos:
  - `rv_app.py` (menu inteiro: abas JOGOS/TV/APPS/OPCOES, SISTEMAS, EXT_JOGO, ORDEM_JOGOS,
    OPCOES_PADRAO dos nucleos, BIOS (bios_arquivos/bios_pasta_nucleo/bios_falta), jogar() que
    monta a linha do rv-jogo, CFG_PADRAO = chaves do retrovolt.cfg).
  - `disco.py` (identifica discos), `tv.py` (RetroTV/mpv, TelaCheia), `video.py`, `wifi.py`,
    `youtube.py` (yt-dlp), `imagens.py`, `fontes.py`, `rv_video_guarda.py`, `dados/`, `licencas/`.
  - Fora do git (vao no zip do pacote): `rv-jogo` (binario), `nucleos/*.so`,
    `nomes_fbneo.json`, `fbneo/hiscore.dat`.
- `rv-jogo/rv-jogo.c` = frontend libretro proprio em C (um arquivo): KMS/DRM 32 bits, tubo
  240p/480i, vsync, CD virtual (leitora fisica), VFS, GLES/Vulkan (Flycast), som ALSA com
  reamostragem, menu do jogo (START+SELECT), convergencia, filtros, run-ahead, DS em 2 HDMI.
  `build.sh` (gera rv-jogo.arm64 e rv-jogo.x86 de teste), `mknucleos.sh <pasta da versao>`
  (baixa e compila todos os nucleos), `aarch64.cmake` (toolchain p/ CMake), `remendos/`.
- `tools/`: mknomes.py (nomes PS1), mknomes_fbneo.py (gera nomes_fbneo.json do DAT do FBNeo),
  fontes, mkedid.py, teste_conv.py. `mockup/`: telas aprovadas.
- Docs: `DESIGN.md` (decisoes + historico por release), `PLANO.md` (hardware/plano tecnico),
  `CONFIG.md` (retrovolt.cfg), `R00_ESTUDO.md`.

## 5. Sistemas e nucleos (R25)
| aba | pasta roms/ | nucleo | fonte (commit usado) |
|---|---|---|---|
| PLAYSTATION | ps1 | pcsx_rearmed | libretro/pcsx_rearmed |
| DREAMCAST | dreamcast | flycast (Vulkan/GLES) | flyinghead/flycast |
| SATURN | saturn | Beetle Saturn v1.32.1 (VFS_HYBRID_SANDBOXED) | libretro/beetle-saturn-libretro |
| SEGA CD / MEGA DRIVE / MASTER SYSTEM | segacd, megadrive, mastersystem | Genesis Plus GX | libretro/Genesis-Plus-GX |
| 32X | 32x | PicoDrive | libretro/picodrive |
| NEO GEO CD | neogeocd | NeoCD (+remendo nomes) | libretro/neocd_libretro |
| PC ENGINE (CD) | pcengine, pcenginecd | Beetle PCE Fast | libretro/beetle-pce-fast-libretro |
| SUPER NINTENDO | snes | Snes9x (cortex-a76) | libretro/snes9x fae2fea |
| NEO GEO / ARCADE | neogeo, arcade | FBNeo rpi5_64 | libretro/FBNeo a836e52 |
| NINTENDO DS | nds | melonDS DS (CPU + JIT) | JesseTG/melonds-ds f394adb |
| GAME BOY ADVANCE | gba | mGBA | libretro/mgba 7a12d6d |
| GAME BOY / COLOR | gb, gbc | Gambatte | libretro/gambatte-libretro 77f2567 |
| NES | nes | Mesen | libretro/Mesen 0102910 |
| NEO GEO POCKET | ngp | Beetle NeoPop | libretro/beetle-ngp-libretro a50d5ac |

## 6. Onde paramos (2026-11-13)
- **R24k** (FBNeo + run-ahead) e **R25** (DS/GBA/GB/NES/NGP; inclui o R24k) entregues como zip.
  O autor AINDA NAO testou no Pi: primeiro passo e pedir o LOG.TXT do teste.
- Coisas a conferir no teste do R25: DS com 2 monitores (LOG "ds: 2 monitores"; tela de baixo
  no 2o HDMI) e com 1 (R3 troca, R2 + analogico direito = toque); Mesen abrindo (a ordem nova
  dos callbacks depois do retro_init vale para TODOS os nucleos: conferir que os antigos
  continuam bons); run-ahead (LOG "run-ahead: ligado"); vsync com 2 monitores.
- Pendentes antigos: barra de progresso do YouTube (overlay ASS) nao aparece no Pi (perguntado
  se as dicas do OSD aparecem no inicio do video; sem resposta); Netflix no Kodi pela chave de
  autenticacao (lado do autor); Dreamcast (tamanho do FBO, reduzir no Vulkan, medir);
  scanlines desiguais em TELA CHEIA no LCD (DESIGN.md "PENDENTE"); ideia de addon RetroVolt no
  Kodi; futuro: lancar tudo pelo cartao em vez do pendrive.

## 7. Refazer os binarios (so com #compilar)
```
sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake
retrovolt-pi/rv-jogo/build.sh
retrovolt-pi/rv-jogo/mknucleos.sh retrovolt-pi/r00/RETROVOLT/versoes/rv23
python3 -B retrovolt-pi/tools/mknomes_fbneo.py "FBNeo/dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat" retrovolt-pi/r00/RETROVOLT/versoes/rv23/nomes_fbneo.json
cp FBNeo/metadata/hiscore.dat retrovolt-pi/r00/RETROVOLT/versoes/rv23/fbneo/
```
- Os binarios precisam de glibc >= 2.38/2.39 (compilados no Ubuntu 24.04, gcc 13); rodam no Pi
  do autor. Pacote: zip com `RETROVOLT/versoes/rv23/` (py + rv-jogo + nucleos + nomes + fbneo/),
  o autor extrai na raiz do pendrive por cima.
- Testes no PC: `RV_FAKEFB=arq.ppm:640x480:32 RV_FRAMES=N rv-jogo.x86 --nucleo X.so --jogo ROM
  --bios D --saves D --nome T --save t [--runahead 1]`; DS em 2 monitores sem HDMI:
  `RV_TESTE_DS2=1920x1080 RV_TESTE_DS2_ARQ=seg.ppm ... --ds-duas-telas 1`. So ROMs de teste
  livres (homebrew/test roms), nunca no git.

## 8. O que tem no zip de backup
- `repositorio.bundle`: o repositorio inteiro com todo o historico (main + a branch).
  Recuperar: `git clone repositorio.bundle ps2tvcrt && cd ps2tvcrt && git checkout claude/new-session-508stw`.
- `RETROVOLT_R25.zip`: ultimo pacote (rv-jogo, nucleos FBNeo/Snes9x/melonDS DS/mGBA/Gambatte/
  Mesen/NeoPop, nomes_fbneo.json, hiscore.dat, menu Python).
- Este RECUPERAR.md.
- NAO tem: os nucleos antigos ja compilados (PCSX, Flycast, Beetle Saturn, GPGX, PicoDrive,
  NeoCD, PCE Fast) nem o transcript da conversa (tem dados privados). Os nucleos antigos estao
  no pendrive do autor (`RETROVOLT/versoes/rv23/nucleos/`) e se refazem com o mknucleos.sh.

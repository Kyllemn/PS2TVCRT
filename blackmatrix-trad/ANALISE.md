# Black/Matrix AD (Dreamcast, JP, v1.002) — analise inicial (Disco 1)

Fonte: copia do autor (.gdi: track01.bin, track02.raw, track03.bin).
Os arquivos do jogo NUNCA entram no git; aqui so ferramentas e notas.

## Disco
- track01: area comum (so 3 TXT de padrao). track02: audio de aviso.
- track03 (GD, LBA 45000): 46 arquivos, **853 MB**.
  - Videos Sofdec (*.SFD): ~400 MB (OPENING, OPDEMO, PROLOGUE, MSxxxx).
  - PCM.DAT 198 MB (vozes/efeitos), EVENT.DAT 97 MB, MAP.DAT 70 MB,
    MAGIC 41 MB, FACE 40 MB, OASOBI 40 MB, CHAR 12 MB, SOUND 13 MB...
  - SCENARIO.DAT 1,9 MB (roteiro), PARAMETA.DAT 2,1 MB (nomes/parametros),
    SYSTEM.DAT 6,7 MB (pacote "PAC"), 1ST_READ.BIN 947 KB (executavel).
- **CD-R: 853 MB > ~700 MB.** Sem arquivo de enchimento. Para o .cdi vai ser
  preciso reduzir ~150 MB (provavel: recodificar os videos SFD). Decidir depois.

## Formatos
- Conteiner **CMP2**: cabecalho "CMP2", contagem em +8, tabela a partir de
  +0x10 com pares (setor inicial, nº de setores), setores de 2048 bytes.
- Roteiro: blocos **"PSI22.00"** = bytecode de script (palavras de 16 bits)
  com o texto embutido: opcode, tamanho (em caracteres), texto em Shift-JIS
  **com bytes trocados** (cada caractere e um u16 little-endian).
  Opcodes de texto vistos: 0x15 (2367x), 0x14 (1894x), 0x1D, 0x20.
  O script tem saltos/deslocamentos: trocar o tamanho do texto exige
  recalcular essas referencias (parser completo do bytecode).
- Texto sem compressao (otimo).

## Volume de texto (estimativa)
- SCENARIO.DAT: 562 blocos; contagem estruturada inicial 4690 falas /
  ~44 mil caracteres (so os opcodes ja entendidos); varredura bruta indica
  bem mais (ate ~150-190 mil caracteres). Numero final depois do parser.
- PARAMETA.DAT: nomes (personagens, itens, magias) — milhares de caracteres.
- 1ST_READ.BIN: menus/mensagens (~2,5 mil caracteres).
- Imagens com texto: a verificar (SYSTEM.DAT/PAC, EVENT, GRAPHIC, PVR).

## Ferramentas (tools/)
- gdls.py: lista os arquivos do track03 (ISO9660 do GD).
- gdx.py: extrai os arquivos (menos SFD e PCM).
- scan.py: varre os blocos PSI22 do SCENARIO e conta as falas.

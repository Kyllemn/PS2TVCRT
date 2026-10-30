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
- Bloco PSI22: "PSI22.00", u32 0, u32 tamanho do codigo (bytes); codigo a
  partir de +0x10; resto do setor em zero (folga para crescer). Blocos so de
  0x11 sao vazios/reservados.
- Instrucao de texto: [op u16][n u16][n caracteres u16]. Valores de 32 bits
  (ex.: enderecos de salto) ficam ALINHADOS em 4 bytes (aparece uma palavra 0
  de enchimento quando o texto tem n par). Enderecos de salto = deslocamento em
  bytes desde o inicio do codigo (ex.: "18 17 3 [0] 2dc" = salta para 0x2dc).
- Proximo passo: achar o interpretador no 1ST_READ.BIN (SH-4; capstone 5 tem
  SH) para ter o tamanho exato de cada opcode e todos os tipos de argumento,
  e so entao escrever o extrator/reinseridor com recalculo dos saltos.
- Gramatica (deduzida dos dados, parser em tools/psi.py, 94 de 501 blocos ja
  fecham certinho no tamanho do codigo):
  - instrucao = [op u16] + argumentos; muitas tem [sub u16] logo apos o op
    (0x08, 0x09, 0x0a...);
  - expressao em notacao polonesa reversa terminada por 0: tag 1 = constante
    u32 alinhada; tags 2/3/4 = variaveis/flags (u16); >= 0x80 = operadores
    (0x90 = igual, 0x96 = e/ou...);
  - 0x02 = se-falso-salta [u32 alvo][expr]; 0x03 = salta [u32 alvo];
  - 0x14/0x15/0x20 = texto [n][n caracteres]; 0x1D = opcao de menu
    [n][texto][u32 alvo]; 0x12/0x17/0x18/0x19 = sem argumentos (0x12 = fim).
- Tabela de comandos no executavel: candidatos testados nao bateram; a
  deducao pelos dados esta rendendo mais.
- Texto sem compressao (otimo).
- Idioma alvo: **portugues do Brasil** (precisa de acentos: a e i o u com
  agudo, a e o com circunflexo, a o com til, c cedilha, a com crase).

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

## Gramatica completa do script (30/10)
- **501 de 501 blocos** do SCENARIO.DAT lidos do inicio ao fim, com todos os
  saltos caindo em inicio de instrucao (tools/learn3.py; formatos em
  tools/psi_spec.json).
- Formatos: E = expressao (RPN terminada em 0; tag 1 = const u32 alinhada,
  2/3/4 = variavel u16, >= 0x80 operador); V = expressao que comeca por
  variavel (atribuicao, op 0x01); S = u16; L = u32 alinhado (salto);
  T = [n][n caracteres].
  Ops com subcomando: 0x08, 0x09, 0x0A, 0x0B. Sem argumentos: 0x0C, 0x12,
  0x17, 0x18, 0x2A. Texto: 0x14, 0x15, 0x20 (T), 0x1D (T + L), (0x08,0x15)
  = E + T (nome de quem fala).
- Roteiro extraido (tools/extrai.py): **10.343 textos, ~200 mil caracteres**
  (3190 FALA op 0x14, 6807 FALA op 0x15, 292 opcoes, 48 perguntas, 6 nomes).
- Codigos especiais no texto: 0xA0FF = quebra de linha (7368x). Varios
  codigos 0x82xx/0x81xx fora do Shift-JIS padrao = **glifos proprios da fonte
  (gaiji)**, ex. {825c} = kanji de "しびれ". Ou seja: o jogo ja usa codigos
  livres para desenhar caracteres proprios -> mesmo caminho para os acentos.
- O texto extraido NAO vai para o git (e texto do jogo); fica so local.

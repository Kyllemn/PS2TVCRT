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

## Fonte (estudo, 31/10)
- Teste t03: o jogo NAO tem meia largura; valores 0x00XX viram outros glifos.
- 1ST_READ.BIN 0x8C05DD2E: codigo Shift-JIS -> indice da fonte:
  0x8140-0x81BF -> codigo-0x8140; 0x8240-0x83BF -> codigo-0x81C0;
  0x889F-0x9872 -> codigo-0x869F (kanji nivel 1); fora disso indice=codigo.
- 0x8C05DD1A: endereco do glifo = *(0x8C1D8318) + indice*128 + 16
  (128 bytes por glifo; 16 de cabecalho).
- 0x8C05D5C4 copia a fonte (memmove) para o buffer; chamada pela rotina
  0x8C047368, que carrega o SYSTEM.DAT (arquivo 11 da tabela de nomes)
  entrada 0 (PAC de 9 secoes) em 0x8C380000. Formato exato ainda nao batido.
- Proximo passo: save state do Flycast para ler a fonte direto da RAM.
- Acentos sao opcionais (decisao do autor); pontuacao . , ? ! ... obrigatoria.

## Fonte localizada (save state do Flycast)
- Save state (.state) = "#RZIP" (blocos zlib); RAM principal dentro dele.
- Fonte "MFD " em RAM 0x8C75F800 = SYSTEM.DAT offset 0x424E0 (entrada 0,
  PAC com deslocamentos em unidades de 2 bytes), 0x90020 bytes, 4608 glifos.
- Glifo: 128 bytes = 32x16 pixels, 2 bits/pixel, textura twiddled em dois
  blocos 16x16 lado a lado, 1o pixel nos bits 7-6 (tools/mfd.py). Tom 1 =
  traco, 2/3 = borda. O jogo roda em 640x240: 32x16 aparece quadrado.
- Solucao: 2 letras por glifo (tools/pares.py, insere2.py). Codigos usados:
  faixa de kanji 0x89xx-0x97xx com byte baixo invalido no Shift-JIS (0x00-0x3F,
  0x7F, 0xFD, 0xFE): ~1000 vagas que nenhum texto japones usa. Se faltar,
  usar kanji que a traducao/menus nao usam.
- Teste t04: 17 falas com acentos, ~38 letras por linha.
- t05/t06: o jogo so desenha as colunas 0-23 do glifo (as 24-31 ficam
  vazias em toda a fonte original) -> 2 letras de 12 colunas; na tela o
  glifo sai ~1,1x na largura e ~2x na altura. "i" desenhado a mao.

## Textos do executavel (1ST_READ.BIN)
- ~200 registros [n u16][n caracteres u16] entre 0x837AC e 0x8A200
  (tools/exe_txt.py): menus, ajuda da configuracao, lojas, avisos de
  memoria, popups ("彼女にします。" 0x891C2, "は　いいいえ" 0x83D3C = 2 campos
  de 3). Linhas de 19 celulas completadas com espaco largo.
- tools/insere_exe.py troca no mesmo lugar (mesmo n); '|' = campos de
  largura fixa. Com os pares (2 letras por celula) o portugues costuma caber.
- Telas como "表示する / 表示しない / 手動設定 / 初期設定" e os titulos
  (CONFIGURATION, MESSAGE...) sao IMAGENS, nao texto: ficam para a fase de
  imagens.

## Fase 1 (textos) — OK
- Patch BlackMatrixAD_D1_PTBR_v1.xdelta, track03 final MD5 46584b093309bea6754d08ae8f0a62dc.
- Ferramentas no commit 0409bb8 (build.sh monta tudo). Testado pelo usuario.
- Travamento ao pular falas rapido acontece tambem no jogo original naquele emulador; em outra plataforma nao trava.
- Pendente para a v2: 英 (0x8970) foi reaproveitado pelo par "ak"; proteger kanji do SYSTEM.DAT
  (tela de nome: かな カナ 英字 戻る 終了 em SYSTEM.DAT 0x1cd8f2) e traduzir para Hira/Kata/ABC/Apag/Fim.

## Fase 2 (imagens) — v2
- Texturas PVRT sem compressao (ARGB1555/RGB565, twiddled): tools/pvr.py le e grava.
- HUD (botoes na tela): EVENT.DAT, PVRT em 0x53310 (unica copia) -> img_hud.py.
- Configuracao (表示する etc.) e itens da ajuda: SYSTEM.DAT 0x6294d0 e 0x6494f0 -> img_sys.py.
- Tela de nome: tabela em SYSTEM.DAT 0x1cd8f0 (rotulos) e 0x1cd9b0 (nomes, [u16 n][7 celulas]) -> nome_sys.py.
- Imagens entre capitulos ficam em japones (decisao do usuario).
- Batalha: CHAR.DAT, PVRT em 0x1350 -> img_bat.py. トウケン=Espada (v3b); a coluna cinza vertical (通潜常在) ficaram sem traduzir.

## Pendencias para a proxima release (acumular, nao gerar patch a cada item)
- Coluna cinza vertical em CHAR.DAT (x176-189, y192-255): 通/潜/常/在 = pedacos de 通常 (Normal) e 潜在 (Poder)
  na versao cinza (opcao desativada). Ver no jogo como e montada antes de traduzir.

## v4 (imagens, parte 2)
- Formato SCL (telas 640x240 de blocos 16x16): tabela u32 40x15 em +0x20; v -> textura v>>16,
  coluna (v>>12)&15, linha (v>>4)&0xff. tools/scl.py monta, remonta e reaproveita blocos iguais.
- Menus do acampamento/loja: SYSTEM 0x21890 (img_menu). Capitulos/Porta/Slot: SYSTEM 0x608810 (img_cap).
- Placas de lugar: EVENT (25 texturas com barra marrom; img_lugar acha pelo conteudo).
- Instrucoes dos minijogos (EVENT blocos 30-33), legendas (218, 219), fins de capitulo (224, 282, 314, 332, 340, 350): img_scl.
- Escolha do mestre: SYSTEM bloco 3 (img_escolha, apaga o japones reconstruindo o fundo).
- Troca de disco: SYSTEM blocos 5 e 6, PAC com duas telas SCL cada (img_disco).
- Relatorio de batalha: PARAMETA 0x1bfdd0 (img_rel).
- Ficam em japones: cartas de abertura de capitulo (EVENT 140-145), icones 縦横突殴鞭弓, coluna cinza 通潜常在.

## Pendencias para a v5 (varredura completa, 31/10)
Ferramentas: tools/varre_jp.py e varre_jp2.py (ignora os pixels das texturas; procura texto que sobrou nos arquivos ja montados).
1. Itens de campo: EVENT.DAT bloco 9 ("IDCO"), 0xe3000. Registros de 0x20 bytes, u16 tamanho em +0x10 e nome logo depois
   (cabe ate 9 celulas). 90 registros, 62 nomes: comidas, flores, roupas, presentes, animais, pratos.
   E dai que vem o "ガーベラ Get!" no campo.
2. 1ST_READ: registros [n][celulas] com o codigo do nome (0xB001) que o exe_txt pulou: 8 mensagens de loja/equipamento
   (descartar, remover, comprar, vender, purificar, despertar), mais "『nome』を入手", "AP insuficiente" e o status com gaiji (paralisia).
3. PARAMETA.DAT: 12 descricoes com gaiji ({825c}, {82f3}{82f4}, {829e}) ou "HP" em ASCII, que o param_txt pulou.
4. Descricao do save no VMU: 1ST_READ 0x86b0a, Shift-JIS normal (big-endian) com katakana de meia largura
   e "章"/"プレイヤー名". O jogo preenche o capitulo e o nome; precisa manter as posicoes.
5. Imagens: cartas de abertura de capitulo (EVENT 140-145), icones 縦横突殴鞭弓 (SYSTEM 0x21890, linha 16), coluna cinza 通潜常在.
Sem texto: SOUND, MAGIC, MAP, OASOBI, FACE, GRAPHIC, CHAR (fora das imagens ja feitas) e o roteiro (100%).
Segunda varredura (31/10, tools/varre3.py): disco inteiro, inclusive PCM.DAT e Shift-JIS nos dois sentidos,
com kanji fora da lista do roteiro e katakana de meia largura. Nao achou nada alem da lista acima.
PCM.DAT so tem audio. Videos SFD: textos em ingles (OPDEMO), sem legenda japonesa gravada na imagem;
as falas dubladas (PROLOGUE, MS*) nao tem legenda nenhuma. Opcional: legendar ao reencodar os videos para o .cdi.

## v5 (31/10) - fecha o disco 1
- Itens de campo: tools/idco.py (EVENT bloco 9, 62 nomes / 90 registros; trad/idco_pt.tsv fora do git).
- Mensagens do exe com o nome do item (0xB001 = 'ｰ' no exe_pt.tsv), AP insuficiente, status de paralisia.
- Save no VMU: tools/vmu.py. Modelo "B.Matrix Cap<n>  PT-BR"; o laco que copiava o nome (celulas da
  fonte do jogo = lixo no menu do Dreamcast) foi desligado: 1ST_READ 0x1f58 'mov #12,r7' -> 'mov #0,r7'.
- 12 descricoes com gaiji: trad/param_extra.tsv (offset do campo, linha1, linha2) lido pelo insere_param.
- Credito na primeira fala das 6 aberturas (SCENARIO blocos 8-13, pos 00d2): trad/especial.tsv
  (bloco, pos, texto), aplicado pelo junta_roteiro. Sem linha vazia ("//" nao existe no roteiro original).
- Tela de titulo: tools/img_titulo.py (SYSTEM, PAC do bloco 1, SCL em 0x11ba00) - credito embaixo do "Disc 1".
- Cartoes de abertura de capitulo: tools/img_abertura.py (EVENT SCL 140-145, inpaint + texto).
- Icones de tipo de ataque: tools/img_icone.py -> VE HO ES IM CH AR (letras 5x7 a mao).
- Fica em japones de proposito: coluna cinza 通潜常在 da batalha (decisao do usuario).
- Verificado: confere.py 0 problemas, tabela CMP2 do SCENARIO intacta, 1356 pares sem colidir com kanji usado,
  arquivos na trilha iguais ao build, patch aplica e bate. Patch v5: md5 a5fcb86cdbf8d7c8b818bed4d59867cb.

## CDI de boot automatico (CD-R), disco 1 (31/10)
- Tamanho: GD tinha 854 MiB; CD-R de 80 min aceita ~680 MiB a partir do LBA 11702. So os videos SFD (354 MiB) dao pra reduzir.
- Videos: tools/sfd_enc.sh (decodifica para YUV cru -> mesma contagem de quadros -> mpeg1video 2 passadas a 1000 kbps,
  maxrate/VBV iguais ao original, GOP 15 IBBP) + tools/sfd_mux.py (mantem as packs de cabecalho SofdecStream e o audio ADX
  byte a byte; pacotes de video de 2018 bytes + 12 de cabecalho como o SFDMUX; PTS/DTS na figura que comeca no pacote,
  DTS de I/P = (ordem+1)*quadro como o original). 354 -> 139 MiB. Conferido: 32 arquivos, audio e cabecalho identicos,
  mesma contagem de quadros.
- Exe (biblioteca gdFs 1.08): duas checagens 'FAD >= 45150' (inicio do GD) em 0x8e754 e 0x8eb20 -> 11852 (11702+150) e
  o FAD do descritor de volume 'CD001' em 0xd2844: 45166 -> 11868. Sem esse terceiro o jogo trava na tela da SEGA
  (v5 do CDI travava; v5b corrige). O monta_cdi confere que nao sobra nenhum 45150/45166.
  Sem checagem de tipo de disco; o jogo abre tudo por nome.
- tools/cdi.py: ISO9660 com base no LBA 11702 (IP.BIN do GD nos setores 0-15), 1ST_READ embaralhado (algoritmo do
  scramble.c do KallistiOS) e CDI audio/dados no layout do cdi4dc (img4dc, SiZiOUS, GPLv3; tabelas de GAP/cabecalho
  vem dele), EDC/ECC Modo 2 Forma 1 vetorizado (conferido contra setores reais do GD).
- tools/monta_cdi.py monta; tools/le_cdi.py le de volta (faixas, ISO, 46 arquivos por md5, 1ST_READ, EDC).
- Resultado: ISO 638 MiB, termina no LBA 338571 (~41 MiB de folga). Patch track03 original -> CDI: 127 MB,
  aplica com xdelta padrao. CDI v5b md5 388eab607eaf698c151d27a9ba2bd7df.
- Logo da Retro Volt na tela de licenca da SEGA: tools/logo_retrovolt.mr (320x90, 14 cores, 7573 bytes, fundo no cinza
  da tela = "transparente"), gerado com tools/mr_logo.py (mesmo formato/compressao do makeip do KallistiOS) e posto pelo
  monta_cdi.py em IP.BIN+0x3820 (area vazia de 8192 bytes no IP.BIN do jogo). So muda os setores 7-10 do disco.
- CDI v6 do disco 1 = v5b + logo. md5 7ddd865fc2c3bd67db6ed714ee4d1f3b. Testes do usuario no v5b: boota no Flycast PC
  com BIOS real; no Flycast do ReplayOS (BIOS real) fica na tela da SEGA - pendente, usuario vai investigar do lado dele.
- CDI v6b: trocado para o layout dados/dados (cdi4dc -d), o mesmo do Volgarr (CDI de referencia do usuario que boota no
  ReplayOS e no console). Sessao 1 = ISO inteiro a partir do LBA 0; sessao 2 = faixa de boot de 302 setores no LBA
  (setores da faixa 1 + 11400) com os 18 primeiros setores do ISO. gdFs: 45150 -> 150 e 45166 -> 166.
  Cabecalho do CDI comparado byte a byte com o do Volgarr: so mudam contagens, LBA da faixa 2, espaco usado e nome.
  tools/le_cdi_dd.py le como a BIOS (sessao 2 -> PVD -> arquivos na sessao 1). md5 ab496ca3c01ad3c8e6a66cd9e5796ad5.
- CDI v6c: IP.BIN tinha a TOC1 do GD em 0x100 ("TOC1" + 0x4100B05E = faixa de dados em FAD 45150); o jogo le dados
  do IP.BIN na memoria (0x8c008000/0x8c008100). Ajustada para 0x41000096 (FAD 150, faixa de dados do CD). v6b bootava no
  Flycast PC mas travava no ReplayOS na passagem da tela da SEGA para o jogo.
- CDI v6d: o IP.BIN do jogo e so do Japao (area "J" e so o texto "For JAPAN,TAIWAN,PHILIPINES."). Comparando com dois CDIs
  que bootam no ReplayOS (Volgarr e Metropolis Street Racer), os dois sao "JUE" com os 3 textos de regiao. Console/BIOS
  em regiao USA para na tela da SEGA (sintoma do ReplayOS). v6d: IP.BIN sem trava de regiao (JUE + 3 textos).
  Obs. do MSR: ele poe a faixa de dados no LBA 45000 (sem patch no exe) e usa subcabecalho 00 00 08 00.
- ReplayOS: v6d boota com BIOS HLE, trava com BIOS real; Volgarr e MSR bootam com BIOS real. Flycast (master) nao trata
  CDI com dados no LBA 45000 como GD (GuessDiscType so ve modo das faixas -> CdRom_XA). Diferencas que sobram contra o MSR:
  codigo de boot do IP.BIN (nosso: Sega 1999; MSR: Sega 2000) e 1ST_READ do MSR gravado sem embaralhar.
  Testes enviados (patches pequenos sobre o v6d): A = boot 0x5820-0x8000 do MSR (BOOT2=...), B = 1ST_READ sem embaralhar
  (SEM_SCRAMBLE=1). monta_cdi aceita essas duas variaveis de ambiente.
- Testes no ReplayOS (BIOS real): A (boot do MSR) reinicia sem parar; B (1ST_READ sem embaralhar) da "Fatal SH4 exception"
  -> a BIOS real desembaralha, e no v6d o jogo comeca a rodar mas trava sem erro (esperando o leitor?).
- Nomes padrao da tela de nome: com pares apareciam quadrados (fonte grande do campo nao tem pares). Agora com letras
  largas do teclado ABC do jogo (Ａｂｅｌ...), max 6: Michet, Vaikun, Jul.
- CDI v6e: formato classico audio/dados (LAYOUT=ad, LBA 11702) + regiao JUE + TOC1 11852 + logo + nomes. Pesquisa:
  procedimento padrao = binhack + scramble + IP.BIN; alguns DC novos recusam audio/dados (dados/dados e preferido).
- CAUSA do travamento com BIOS real: a gdFs 1.08, ao montar o disco, pede o status do leitor e exige tipo de disco 0x80
  (GD-ROM): 0x8c09dfb0 'mov.w #0x80; cmp/eq; bt ok; bra erro'. CD-R informa 0x20 (CD-ROM XA) -> erro de montagem, o jogo
  fica tentando de novo (sem crash). HLE responde diferente e passa; savestate passa porque a montagem ja foi feita.
  Teste de regiao (core em Japan) nao mudou nada. v7: 'bra erro' (0x8dfb8, a0f4) -> nop. Layout dados/dados + regiao JUE
  + TOC1 + logo + nomes. Exe do CDI difere do GDI so em 0x8dfb8, 0x8e754, 0x8eb20, 0xd2844.
- New Game no CDI v7 (BIOS real, ReplayOS): tela escurece e volta ao titulo. GDI traduzido com BIOS real: OK. CDI com os
  videos ORIGINAIS: mesmo erro -> nao sao os videos. Causa: checagem de disco do jogo de 2 discos (tarefa 0x8c046fce):
  chama a BIOS (vetor 0x8c0000e0, r4=2, funcao 0x8c0a25a4) que copia o cabecalho do disco no leitor para 0x8c008100;
  depois compara "1/2"/produto com o do disco de boot (0x8c008000). Em CD-R a BIOS falha. v8: 0x925a4 -> rts; mov #0,r0
  e literal 0x92568 (0x8c008100) -> 0x8c008000. Na troca para o disco 2 o jogo nao vai aceitar o disco 2 sozinho:
  reiniciar e dar boot pelo disco 2 (que carrega o save).
- CDI v8 TESTADO pelo usuario no ReplayOS com BIOS real: boot, New Game, videos e dialogos passados rapido - tudo OK.
  v8 = candidato final do disco 1 (md5 10d24e80f05af847523e41a38b996376). GDI correspondente: patch GDI_v7.

## Disco 2
- Arquivos iguais ao disco 1 (original): SCENARIO (todo o roteiro dos dois discos), 1ST_READ, PARAMETA, CHAR, FACE, OASOBI,
  MAGIC, GRAPHIC, SOUND -> usam direto os traduzidos do disco 1 (mesmo mapa de pares e mesma fonte).
- SYSTEM: blocos 0,2,5,6,7,8 iguais ao d1 (em outras posicoes) -> tools/transplanta_blocos.py copia os blocos traduzidos
  (fonte, tela de nome, menus, capitulos/slots, troca de disco). Bloco 1 = titulo "Disc 2" (img_titulo, mesmo layout).
  Bloco 3 = outra tela de escolha do mestre (fundo branco) -> tools/img_escolha_d2.py. Bloco 4 = retratos (texto em
  ingles), bloco 9 = PVX com numeros/"Normal!!/Advanced!!" (ingles).
- EVENT do d2 e outro arquivo (622 blocos, 314 vazios): sem IDCO, sem o HUD do d1 (procurado por pixel em todos os
  arquivos do d2), sem minijogos. Tem: 24 placas de lugar (img_lugar com LUGARES=trad/lugares_d2.txt), cartoes de
  capitulo 7-11 + final nos SCL 146-151 (img_abertura com CAPITULOS=trad/capitulos_d2.json e AREA_X=56,584), fins de
  capitulo 380/402/431/488/499 e 52 telas pretas do final com texto vertical (tools/img_fim_d2.py, uma versao por mestre).
- Varredura de texto (varre_jp2/varre3) em EVENT/SYSTEM/MAP do d2: so a tela de nome (bloco 2, ja transplantado).
- track03 do d2: grava_track com tools/lista_d2.txt. Patch GDI d2 v1.
- Videos do d2: 32 SFD a 1500 kbps (sobra espaco: resto do disco = 411 MiB).

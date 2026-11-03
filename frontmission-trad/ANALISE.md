# Front Mission 1st (PS1, SLPM-87317): tradução usando o roteiro do DS

Projeto pessoal. Nenhum arquivo do jogo nem texto extraído entra no git: só ferramentas e notas.
Os arquivos do jogo (imagem PS1 e ROM DS do usuário) ficam fora do repositório.

## Disco PS1
- Arquivos: SLPM_873.17 (exe, carregado em 0x80010000), PACKAGE.PAK (59 entradas), MAP16/MAP7/SIDE.DAT, GR_ALL.BIN,
  SE.DAT, BGM.DAT, MOVIE.STR, DUMMY.BIN (27 MB de enchimento no fim do disco).
- O exe acha os arquivos pelo nome (`\PACKAGE.PAK;1`), então o PAK pode mudar de lugar: o PAK novo vai para o
  início do DUMMY.BIN e o diretório do CD é atualizado (`isopatch.py`, `build_disc.py`).
- PACKAGE.PAK: palavra 4 em diante = pares (offset, tamanho) alinhados a 2048.
- Compressão **Wp16** (Square): flags de 32 bits LSB primeiro (1=literal de 16 bits), backref `d<<5|c`
  (d palavras para trás, 0=2048; c+2 palavras), janela inicial de 2048 palavras zeradas (`wp16.py`, ida e volta exata).

## Texto PS1
- Fonte: entrada 51 do PAK, 1440 glifos 16x16 = 2 tiles 8x16, 2 bits/pixel (2=traço, 3=contorno).
  A fonte é ordenada por frequência; a tabela glifo->caractere foi transcrita à mão (fica fora do git).
- Código: 00-BF = glifo; C0-C6 + b = glifo 0xC0+(p-0xC0)*0xC0+b; FE = quebra; FF = fim; FC xx = nome do personagem xx;
  F8/F9/FA/FD xx = controles (velocidade, pausa, posição, símbolos).
- O motor quebra a linha sozinho a cada 15 glifos.
- Missões: entradas 23-33 do PAK, carregadas em 0x80198918; o script aponta para as falas com `c0 19 LL HH`
  (endereço 0x8019HHLL). As falas ficam num bloco contínuo no meio do arquivo. Limite: o arquivo não pode passar de 0x801A0000.
- Cidades: entrada 41 (dat_city, 0x801B0000) = código + texto. As falas são referenciadas pelos scripts de cidade
  42 (OCU) e 43 (UCS) com `c0 1b LL HH` (0x801BHHLL), o mesmo esquema das missões. Limite: 64 KB (0x801C0000).
  No DS, as referências equivalentes estão nos overlays 15 (OCU) e 22 (UCS), apontando para as strings do overlay 37.
- Interface: entrada 22 (0x80191000, até 0x7918) = 1128 strings (menus, ajuda, nomes, fichas de peças/armas, grade de
  kana da tela de nome). Referências: ponteiros u32 (game.bin e outros), `c0 19` nos scripts e pares lui/addiu no
  código (incluindo o exe). Estratégia: traduzir dentro do espaço original quando couber; só mover o que não couber.
- Fichas de equipamento = "NOME：" + ícones/números (FD xx); só o nome muda. Nomes oficiais do DS em tabelas
  ASCII terminadas em '@' (arm9/overlays).
- Nomes: entrada 22 (0x80191000) tem opções de menu, nomes padrão e a grade de kana da tela de nome;
  o game.bin (entrada 0) tem 19 ponteiros para os nomes em 0x75AF0.

## Roteiro do DS (Front Mission, EUA)
- O DS reescreveu o jogo em C a partir do código do PS1; a ROM ainda traz os arquivos do PS1 em `Original/Output`.
- O texto inglês fica nos overlays (37 = cidades, 38-48 = missões 23-33 do PS1) e no arm9.
- Codificação: 00-19 = A-Z, 1A-33 = a-z, 40-49 = 0-9, 4A ':', 4F '?', 50 espaço, 51 '!', 52 '"', 57 ''', 5C ',', 5D '-', 5E '.',
  FE/FF e controles iguais aos do PS1.
- As falas ficam agrupadas por tamanho; a ordem do script se recupera lendo os ponteiros na ordem das tabelas de
  literais do código ARM (`fmlib.ds_sequence`).

## Casamento PS1 x DS
- Alinhamento global (Needleman-Wunsch) por quem fala (nomes aprendidos), linhas, pontuação, tamanho e significado
  (dicionário JMdict, CC BY-SA, segmentação por casamento mais longo) + busca local/global para os de baixa nota.
- Amostra: ~90% corretos nos de nota alta. Revisão manual planejada.

## Inglês no PS1 sem mexer no código
- Fonte de pares: cada glifo 16x16 recebe duas letras 8x16 (estilo da fonte original, base Unifont).
  ~870 pares para as missões, 1329 espaços livres (reservados: ícones e letras/números latinos da interface).
- Quebra de linha refeita em 30 colunas (`en_enc.rewrap`).
- Teste no emulador (PCSX-ReARMed libretro + frontend ctypes, BIOS do próprio usuário): briefing e diálogos
  da missão 1 aparecem corretos em inglês.

## Estado (teste no emulador)
- Missão 1 e loja da primeira cidade em inglês funcionando com o disco reconstruído (falas idênticas compartilham
  posição para caber no limite de memória).

## Pendências
- Tela de nome (grade de kana) e nome padrão.
- Cidades (como as cenas são encontradas), menus/UI (entrada 22, game.bin, setup/svld/battle), demo, exe.
- Revisão manual do casamento; depois PT-BR.

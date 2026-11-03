# Front Mission 2 (PS1, SLPM-87331 v1.1) — PT-BR sobre o patch ingles EXT 1.1

Base: imagem japonesa v1.1 (bin de 426.215.328 bytes) + FM2_ENG_EXT.xdelta (Numkl, 03/10/2026).
Portugues sem acentos (a fonte do BIOS so tem ASCII); nao mexe em fonte nem no executavel.

## Conteiners (ZDATA.BIN, IDATA.BIN)
u32 n, u32 setores de cabecalho, n pares (setor, tamanho). Subarquivos alinhados a 2048.

## Compressao (rotina 0x8001EA30 do SLPM_873.31)
u32 tam. descomprimido, u32 tam. comprimido; comando c:
00-3F literais (c&3F)+1 | 40-7F repete byte (c&3F)+3 | 80-BF copia (c&3F)+2, dist=1 byte | C0-FF copia (c&3F)+2, dist=u16.
Fim alinhado a 4. fm2lz.compress faz ida e volta exata nos 471 subarquivos comprimidos.

## Texto
`{...}` = modo ASCII, `|` = quebra de linha, `?!` = nova pagina, `@arex` etc. = nomes, fora das chaves = Shift-JIS.
- ZDATA: o patch ingles anexou o texto no fim de cada subarquivo e trocou ponteiros u32.
  Ponteiro conhecido = palavra que difere entre JP e EN e aponta para o anexo.
  Texto maior que o espaco so e realocado se o ponteiro for conhecido; senao precisa caber.
- IDATA: traducao no lugar, sem ponteiros alterados (capacidade fixa).
- Capacidade segura = max(tamanho EN, tamanho do texto JP original na mesma posicao).
- Cada subarquivo do ZDATA fica no mesmo setor da versao inglesa (nada muda de lugar no disco).

## Teste
Abertura (IDATA sub 116) em portugues verificada no emulador: texto aparece e a cena segue normal.

## Andamento da traducao (fluxo)
- pt/I_<bloco>.tsv e pt/Z_<bloco>.tsv: `offset<TAB>texto PT` (sem acentos). build_pt.py valida e gera fm2_pt.bin.
- Validador: codigos ($xx, @nome, bytes de controle) iguais ao EN ou ao JP; largura <= max(EN,24); linhas/pagina <= max(EN,JP); tamanho <= espaco (IDATA) ou realocacao por ponteiro conhecido (ZDATA).
- ZDATA: realocacao usa primeiro o espaco JP morto (texto JP intacto e sem nenhuma referencia u32), depois o fim do bloco.
- Ferramentas: falta.py (pendentes de um bloco), autotr.py (aplica dicionario EN->PT), propagate.py/alts.py (repetidas), cobertura.py.
- Dicionarios EN->PT (tr*.py, zlocs.py, ztown.py) ficam fora do git por conterem texto do jogo.
- IDATA: completo (missoes, cenas, tutoriais). ZDATA: feitos 9-69 (locais, briefings), 202-206 e ajuda da rede; faltam 207-251 e 309-359.
- Nomes corrigidos pelo JP: bloco 147 e Crow (nao Royd); capangas do Crow.

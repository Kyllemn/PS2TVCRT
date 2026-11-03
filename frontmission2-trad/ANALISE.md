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

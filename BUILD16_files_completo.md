# FastLoader Build 16: manipulação completa de arquivos no FILES

Data: 01/10/2026
Arquivo: `fastLoader.elf` (md5 `8ce9c0e9c57e8155350be294785fc5ff`)
Status: compilado sem erros, ainda não testado no console. Inclui tudo do Build 15.

## O que mudou

O FILES agora trabalha com vários itens de uma vez, entre quaisquer dispositivos: HD, USB, MX4SIO, MMCE, MC 1 e MC 2.

### Seleção

- **Círculo** marca ou desmarca o item do cursor. Os marcados aparecem com `* ` antes do nome.
- O rodapé mostra quantos itens estão marcados e o que está copiado. Exemplo: `R1 PARA ACOES - 3 MARCADO(S) - COPIADO: 5 ITENS`.
- Sem nenhum marcado, as ações valem para o item do cursor, como antes.

### Menu do R1 (11 opções)

| Opção | O que faz |
| --- | --- |
| Recortar | Guarda os marcados (ou o item do cursor) para mover. Abre a escolha do dispositivo de destino. |
| Copiar | Igual, mas para copiar. |
| Colar | Cola tudo na pasta aberta. |
| Excluir | Apaga os marcados. Pastas são apagadas com tudo dentro. Pede confirmação: `EXCLUIR N ITENS?` |
| Renomear | Renomeia de verdade o arquivo ou a pasta do cursor, pelo teclado da tela. |
| Nova pasta | Cria uma pasta na pasta aberta, pelo teclado da tela. |
| Marcar todos | Marca todos os itens da pasta. |
| Desmarcar todos | Tira todas as marcas. |
| Inverter marcação | O que estava marcado sai e o resto entra. |
| Propriedades | Tipo, tamanho e local. Para pastas, quantos arquivos e subpastas têm dentro e o tamanho total. Com vários marcados, a soma de tudo. |
| Limpar copiados | Esvazia a área de transferência. |

- O submenu mostra no topo quantos itens estão marcados.
- As opções de Copiar, Recortar, Excluir e Propriedades mostram a quantidade, por exemplo `Copiar (4)`.
- Opções que não fazem nada no momento ficam apagadas. Exemplo: Colar sem nada copiado.

### Colar

- **Barra de progresso do conjunto:** `Copiando 3 de 12` ou `Movendo 3 de 12`, com o nome do arquivo atual e os MB do total.
- **Cancelar:** Círculo cancela tudo. O arquivo que estava pela metade é apagado.
- **Itens que já existem no destino:** a pergunta aparece uma vez só, para o lote inteiro.

  | Botão | Ação |
  | --- | --- |
  | X | Substituir todos |
  | Quadrado | Pular os que já existem |
  | Círculo | Cancelar |

  - Pastas de mesmo nome são juntadas.
  - Com um item só, a pergunta é a mesma de antes: `SUBSTITUIR [NOME]?`
- **Resumo no final:** quantos itens foram colados, pulados e com erro, e o motivo do último erro.
- **Colar na mesma pasta de onde copiou:** cria uma cópia `NOME - Copia.EXT`, ou `NOME - Copia 2.EXT` se a primeira já existir.
- **Recortar:**
  - No mesmo dispositivo, apenas renomeia. É instantâneo, mesmo com ISOs grandes.
  - Entre dispositivos, copia e só depois apaga o original.
  - Os itens movidos saem da área de transferência. Os que deram erro continuam nela para tentar de novo.
- **Pasta dentro dela mesma:** colar uma pasta dentro dela mesma ou numa subpasta dela é recusado, só para aquele item.
- **Memory card:** nomes com mais de 31 caracteres não cabem. A cópia avisa erro de criação e o Renomear avisa antes.

### Excluir

- Tela `Excluindo 2 de 5`.
- Resumo de apagados e erros.
- O que for apagado sai da área de transferência.

### Renomear e Nova pasta (teclado da tela)

- **Nova linha de símbolos:** `( ) [ ] ! & ' + , #`, para nomes como `Jogo (USA).iso`.
- **Minúsculas:** botão `abc`/`ABC` na fileira de ações, ou **Quadrado**.
- **Mover o cursor no texto:** L1/R1, além dos botões `<` e `>`.
- **Nome que já existe:** o loader avisa, e o teclado continua aberto com o que foi digitado.
- **Trocar só maiúscula ou minúscula** (`jogo.iso` → `JOGO.ISO`): funciona. O loader passa por um nome temporário, porque o FAT considera os dois o mesmo nome.
- **Depois de renomear ou criar:** o cursor vai para o item.
- **Renomear jogo nas abas de jogos:** continua igual (nome de exibição). Ganhou as minúsculas e os símbolos.

## Funções

| Parte | Funções e itens |
| --- | --- |
| Motor | `files_paste_all`, `files_paste_conflicts`, `files_delete_selection`, `files_copy_name`, `files_clip_forget`, `files_child`, `files_tree_stats`, `files_file_size`, `files_fmt_size`, `files_wait_frame`, `files_name_max_for` |
| Área de transferência | `files_clip_dir`, `files_clip_names[]`, `files_clip_dirs[]`, `files_clip_count`. Substitui `files_clip_full`, `files_clip_name` e `files_clip_is_dir`. |
| Menu | `enum FM_*`, `fm_names`, `files_menu_enabled` |
| Teclado | `kb_mode` (0 jogo, 1 renomear arquivo, 2 nova pasta), `kb_lower`, `kb_rows_up`/`kb_rows_lo`, `KB_ROWS 5`, `KB_ACT_COUNT 6` |
| Popups | Propriedades (`show_files_props`). "Já existe" com 3 botões. Excluir com N itens. |

## Teste feito no PC

O motor de colar e excluir foi compilado no PC com um sistema de arquivos simulado, com o mesmo código da build. Casos verificados:

- copiar 2 itens entre dispositivos;
- pular existentes;
- cópia na mesma pasta;
- pasta dentro dela mesma (recusada);
- recortar para o memory card;
- recortar no mesmo dispositivo;
- excluir com a área de transferência afetada;
- contagem das Propriedades.

## Como voltar atrás

Em `restauracao/pontos_de_restauracao.zip`:

| Arquivo | Volta para |
| --- | --- |
| `main_before_filesfull.c` | Build 15: um item por vez, menu com 4 opções |

## Teste sugerido no console

- [ ] Marcar 3 arquivos com Círculo, Copiar, escolher outro dispositivo e Colar. A barra mostra "Copiando 1 de 3".
- [ ] Colar de novo no mesmo lugar: aparece "3 DE 3 ITENS JÁ EXISTEM". Testar Quadrado (pular) e X (substituir).
- [ ] Copiar e colar na mesma pasta: cria "NOME - Copia".
- [ ] Recortar uma ISO grande dentro do HD para outra pasta: instantâneo.
- [ ] Recortar do USB para o HD: copia e apaga do USB.
- [ ] Copiar um save do MC 1 para o USB e de volta para o MC 2.
- [ ] Excluir 2 itens marcados.
- [ ] Renomear um arquivo, com minúsculas e parênteses.
- [ ] Criar uma nova pasta.
- [ ] Propriedades de uma pasta e de vários itens.
- [ ] Marcar todos, Inverter marcação e Limpar copiados.

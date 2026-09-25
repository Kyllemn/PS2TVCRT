# RetroLoader

RetroLoader é um launcher de ISOs e apps para PS2, feito para atender às demandas de TVs CRT no mercado. Letras maiores, fácil navegação, para pegar e jogar. Execute seus jogos diretamente da raiz de qualquer dispositivo. Navegue fácil pelos menus. Bem-vindo ao mundo CRT dos anos 2000, a era de ouro do PS2. Usa o Neutrino para o boot/chainload dos jogos.

## Funcionalidades

- **Favoritos**: marque seus jogos mais jogados e acesse direto sem navegar pela lista completa — algo que o OPL não oferece
- **Confirmação de boot**: popup de confirmação antes de iniciar um jogo, inclusive pela tela de favoritos, evitando boots acidentais
- **Modo de compatibilidade por jogo**: fixe o modo de compatibilidade de cada título individualmente, sem precisar reconfigurar toda vez
- **Suporte a múltiplos dispositivos**: HD, USB, MX4SIO e MMCE (dois slots), com detecção automática do que está conectado
- **Boot direto do memory card**: também é possível rodar a partir do mc0:, sem depender só de HD/USB
- **Interface própria**, sem depender de temas ou skins de terceiros
- **Assets embutidos no próprio `.elf`**: nada de arquivos soltos de imagem/ícone espalhados pelo dispositivo — um único arquivo já traz tudo

## Instalação

1. Copie `RetroLoader.elf` para a raiz do seu dispositivo de armazenamento (USB/HD/MX4SIO/memory card).
2. Garanta que o Neutrino esteja instalado junto, na seguinte estrutura:

```
/
├── RetroLoader.elf
└── FastLoader/
    ├── neutrino.elf
    ├── config/
    └── modules/
```

3. Inicie o `RetroLoader.elf` pelo seu método de boot habitual (OPL, browser, etc), ou renomeie para `BOOT.ELF` e substitua de vez os boots do seu PS2. Dentro dele você acessa e inicia seus arquivos `.ELF` e `.ISO` diretamente.

# RetroLoader — Guia de Controles e Menus

Este guia explica todos os botões, telas e formatos de arquivo suportados pelo RetroLoader.

## Navegação principal

| Botão | Ação |
|---|---|
| **Cima / Baixo** | Navega pela lista de jogos/apps |
| **L1 / R1** | Troca de aba entre os dispositivos (HD → USB → MX4SIO → MMCE → FILES). Trava nas pontas — L1 para no HD, R1 para no FILES, não dá a volta |
| **L2 / R2** | Pula uma página inteira pra cima/baixo na lista (útil em listas grandes) |
| **X** | Abre a confirmação de boot do item selecionado ("Iniciar [nome]?") |
| **Círculo** | Sai para o menu do Neutrino (pede confirmação antes) |
| **Quadrado** | Abre o popup de **Modo de Compatibilidade** do jogo selecionado |
| **Triângulo** | Abre o teclado virtual pra **renomear** o item selecionado |
| **Esquerda** | Adiciona o item selecionado aos **Favoritos** (não remove — pra remover, use a tela de Favoritos) |
| **Select** | Atualiza a lista do dispositivo atual (rescan) e sorteia um novo fundo de tela |
| **Start** | Abre o menu de **Opções** |
| **R3** | Troca o driver ativo entre MX4SIO e MMCE (útil quando os dois estão conectados e só um pode estar ativo por vez) |
| **Start + Select (juntos)** | Abre a tela de **Favoritos** |

## Tela de Favoritos (Start + Select)

| Botão | Ação |
|---|---|
| **Cima / Baixo** | Navega entre os favoritos |
| **X** | Abre a confirmação de boot ("Iniciar [nome]?") — o jogo só inicia depois de confirmar |
| **Triângulo** | Remove o item da lista de favoritos |
| **Círculo** | Fecha a tela sem fazer nada |

Limite: até 10 jogos favoritados por vez.

## Popup de confirmação ("Iniciar [nome]?")

| Botão | Ação |
|---|---|
| **Esquerda / Direita** | Move o cursor entre SIM / NÃO |
| **X** | Confirma a opção selecionada — se for SIM, inicia o boot de verdade |
| **Círculo** | Cancela e fecha o popup (atalho, equivale a escolher NÃO) |

## Popup de Modo de Compatibilidade (Quadrado sobre um jogo)

Permite marcar até 6 flags de compatibilidade (`-gc=`) usadas pelo Neutrino no boot daquele jogo específico.

| Botão | Ação |
|---|---|
| **Cima / Baixo** | Navega entre os 6 modos |
| **X** | Marca/desmarca o modo selecionado |
| **Triângulo** | Confirma e salva a seleção pra esse jogo |
| **Círculo** | Cancela sem salvar |

## Teclado virtual (Triângulo → Renomear)

| Botão | Ação |
|---|---|
| **Direcionais** | Move o cursor pelo teclado na tela |
| **X** | Digita o caractere selecionado, ou executa a ação da última fileira (DEL / ◀ / ▶ / OK / CANCELAR) |
| **Círculo** | Cancela a qualquer momento, sem salvar |

## Menu de Opções (Start)

| Botão | Ação |
|---|---|
| **Cima / Baixo** | Navega entre Desligar / Reiniciar / Créditos |
| **X** | Confirma a opção selecionada |
| **Círculo** | Fecha e volta pra interface |

## Gerenciador de Arquivos (aba FILES)

Dentro do FILES, os botões trocam de função (esquema uLaunchELF/wLaunchELF):

| Botão | Ação |
|---|---|
| **X** | Abre pasta / seleciona arquivo |
| **Círculo** | Marca/desmarca o item atual (seleção múltipla) — **não** sai mais do app aqui dentro |
| **Triângulo** | Sobe um nível de pasta — **não** abre mais o renomear aqui dentro |
| **R1** | Abre o submenu Recortar / Copiar / Colar / Excluir |
| **Select** | Sai do FILES e volta pra aba MMCE |
| **L1/R1 (troca de aba)** | Travado enquanto estiver dentro do FILES |

Ao entrar na aba FILES pela primeira vez, um popup pede pra escolher o dispositivo (HD/USB/MMCE/MX4SIO) antes de mostrar qualquer lista.

## Formatos de arquivo suportados

| Dispositivo/Aba | Formato aceito | Observação |
|---|---|---|
| **HD / USB / MX4SIO / MMCE** | `.ISO` | Jogos de PS2, iniciados via Neutrino |
| **APPS** | `.ELF` | Aplicativos/homebrew |
| **FILES** | Navegação livre de pastas/arquivos | Recortar/Copiar/Colar/Excluir ainda não implementados |
| **CDVD** | Disco físico no drive | Detecção automática com popup de confirmação |
| **Memory Card (mc0)** | Também suportado como origem de busca de jogos | Além de HD/USB/MX4SIO/MMCE |

## Estrutura de pastas exigida

```
/
├── RetroLoader.elf   (ou BOOT.ELF, se substituir o boot padrão)
└── FastLoader/
    ├── neutrino.elf
    ├── config/
    └── modules/
```

## Compatibilidade

O boot dos jogos de PS2 é feito via **Neutrino**, então a compatibilidade de jogos segue a mesma lista do Neutrino. Caso um jogo específico precise de um modo de compatibilidade diferente do padrão, use o popup de **Modo de Compatibilidade** (Quadrado) pra ajustar antes de iniciar — a configuração fica salva por jogo, não precisa repetir toda vez.


## Notas


- O código-fonte deste projeto é fechado; apenas builds compilados são publicados aqui, pelas Releases.
- O Neutrino é um projeto open-source independente — consulte o [repositório oficial dele](https://github.com/rickgaiser/neutrino) para mais detalhes.

# RETROVOLT Pi — decisoes de design (aprovadas pelo autor)

Raspberry Pi 5 + leitora USB (ASUS SDRW-08D1S-U), saida HDMI (LCD ou conversor
HDMI > componente para TV de tubo). Detalhes tecnicos e fases ficam no plano.

## Tema escolhido: "Editorial" (mockups em `mockup/rv_F*.png`)
- Base 320x240 (240p nativo no tubo); fonte 8x8 (a mesma do PS2).
- Fundo azul-marinho (12,18,40); texto marfim (240,236,225); inativo cinza-azulado
  (120,130,160); destaque laranja (255,140,40).
- Tarja laranja vertical nos DOIS lados da tela.
- Topo: "RETROVOLT" a esquerda, relogio a direita.
- Ao lado (a esquerda) do relogio: icone de Wi-Fi + nome da rede quando
  conectado; "SEM WI-FI" quando nao conectado. Em fonte pequena (3x5).
- Embaixo do relogio (alinhado a direita, cinza mais apagado, fonte pequena
  3x5 para nao poluir), 3 linhas:
  temperatura + uso da CPU ("47C  CPU 12%"), memoria ("RAM 1.2/4GB") e o
  dispositivo de jogos com o espaco usado ("USB 41% USADO" ou "SD ...").
  O dispositivo (SD ou pendrive) e escolhido em OPCOES > PASTAS DE JOGOS.
- Abas DISCO / JOGOS / OPCOES, trocadas com L1/R1, SEM numeros, fonte normal
  (8x8, mesmo tamanho do resto); aba ativa em marfim com sublinhado laranja.
- Listas uma embaixo da outra, sem numeros e sem contagem de itens.
- Item selecionado: marca laranja a esquerda, nome em marfim sublinhado em
  laranja e a seta "->" LOGO DEPOIS DO NOME (nao na ponta direita).
- Rodape: "L1/R1 ABAS" a esquerda e os botoes da tela a direita.
- Icone do CD: liso, iridescente, com brilho e sombra (nao pixelado).

## Abas
1. DISCO: CD grande a esquerda; a direita "DISCO NA LEITORA", console, nome do
   jogo, codigo, nucleo e botao "X JOGAR ->".
2. JOGOS: so aparecem as pastas dos sistemas que tem jogos na pasta de jogos.
   X entra na pasta e lista os jogos (titulo do sistema em laranja + posicao N/M).
   **Area da imagem do console:** na lista de sistemas, o espaco vazio a direita
   (aprox. x 172-292, y 66-202 em 320x240) mostra uma imagem em ALTA RESOLUCAO
   do console selecionado, SEM moldura, trocando conforme a selecao.
   As imagens serao enviadas pelo autor no futuro.
3. OPCOES: volume geral, video, convergencia (laterais amarelas), controles,
   Wi-Fi, leitora, pastas de jogos, sistema, REINICIAR e DESLIGAR (vermelho).

## Menu dentro do jogo (a desenhar)
Opcoes do nucleo, volume do nucleo, botoes, convergencia, salvar configuracao,
restaurar padrao.

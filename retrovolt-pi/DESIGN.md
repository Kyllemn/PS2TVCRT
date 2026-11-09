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
- Abas DISCO / JOGOS / TV / OPCOES, trocadas com L1/R1, SEM numeros, fonte normal
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
3. TV (RetroTV, mockup `rv_F5_tv.png`): lista das "grades" de canais (cada
   arquivo .txt/.m3u da pasta TV e uma grade; cada uma lembra o ultimo canal).
   A direita: mini TV de tubo com barras de cor e "CH nn", ULTIMO CANAL,
   AGORA / DEPOIS (do guia). Embaixo da lista: total de canais e estado do guia.
   X ASSISTIR abre o RetroTV na grade escolhida (dentro dele vale a interface
   propria do RetroTV: banner, guia, lista de canais, estatica, NO SIGNAL).
4. OPCOES: volume geral, video, convergencia (laterais amarelas), controles,
   Wi-Fi, leitora, pastas de jogos, sistema, INFO, REINICIAR e DESLIGAR (vermelho).
   (Status do sistema saiu de baixo do relogio; mockups atualizados.)
   INFO: temperatura, uso da CPU, memoria (RAM) e o dispositivo de jogos
   (SD ou pendrive, escolhido em PASTAS DE JOGOS) com o espaco usado.
   (Essas informacoes NAO ficam mais embaixo do relogio.)

## Menu dentro do jogo (a desenhar)
Opcoes do nucleo, volume do nucleo, botoes, convergencia, salvar configuracao,
restaurar padrao.

## START+SELECT dentro do jogo (aprovado; mockup `rv_F6_popup.png`)
- Toque simples (sem segurar). Abre o MENU PRINCIPAL como popup por cima do
  jogo: o jogo pausa e a imagem dele fica atras, escurecida.
- Aparece a aba JOGO (so quando ha jogo rodando): CONTINUAR, REINICIAR JOGO,
  OPCOES DO JOGO. As outras abas (DISCO/JOGOS/TV/OPCOES) funcionam normalmente.
  Reiniciar e sempre por aqui (combinacoes de reset dos jogos nao sao necessarias).
- START+SELECT de novo (ou CONTINUAR) volta ao jogo.
- Abrir outro jogo/nucleo fecha o atual NA HORA (sem save state). Antes de
  fechar, so grava o save do proprio jogo (memory card / bateria), em milissegundos.

## Sistema automatico: o minimo de configuracao (proposta)
Principio: o RetroVolt decide sozinho tudo o que da para detectar; a pessoa so
mexe no basico da jogatina e em filtros.

Tela (detectada ao ligar e ao trocar o cabo, pela EDID):
- TUBO (15 kHz): menu em 320x240 (240p); jogos na resolucao nativa de cada
  console (aviso de 5 s no rodape).
- LCD: usa a resolucao nativa e a proporcao do monitor. O menu e desenhado na
  largura da proporcao (4:3 = 320, 16:10 = 384, 16:9 = 426 pontos logicos, sempre
  240 de altura) e ampliado em escala inteira, nitido (mockup `rv_F7_disco_wide.png`).
- Conversores HDMI>VGA/componente podem nao informar a EDID certa: se a deteccao
  ficar em duvida, o primeiro boot pergunta UMA vez "TUBO ou LCD?" e lembra.
  Modo seguro ao ligar segurando SELECT continua valendo.
Jogos no LCD:
- Proporcao original do console (ex.: 4:3) centralizada, laterais pretas; escala
  inteira na vertical quando couber (sem tremido de pixels).
- 50 Hz para jogos PAL e 60 Hz para NTSC quando o monitor aceita; senao 60 Hz.
Escolhas feitas por nos (sem menu para isso):
- Um nucleo por sistema, o melhor para o Pi 5.
- Regiao/BIOS pelo proprio disco ou ROM.
- Controles reconhecidos sozinhos (base de controles do SDL); remapear e opcional.
Opcoes que ficam para a pessoa:
- VIDEO: SAIDA (AUTO / TUBO / LCD); FILTRO (NENHUM / SCANLINES / CRT, so no LCD);
  IMAGEM (ORIGINAL / TELA CHEIA).
- Volume, convergencia (ja existente), e por jogo: OPCOES DO JOGO.

## PENDENTE (pedido do autor, fazer depois)
- (feito no R04) Titulo "RETRO" em marfim e "VOLT" em laranja.
- (feito no R03/R04) Fonte escolhida: 6X10 (X11 misc-fixed). Fontes lisas (anti-aliasing) ficam para depois.

## Menu do core (popup START+SELECT): SAVE STATE / LOAD STATE (aprovado)
- Aba JOGO passa a ter: CONTINUAR, SAVE STATE, LOAD STATE, REINICIAR JOGO,
  OPCOES DO JOGO.
- 10 espacos (slots) por jogo, para salvar e para carregar.
- Cada slot guarda junto uma miniatura (screenshot) do jogo no momento do save,
  mais data/hora. A captura e feita por dentro do nucleo (ultimo quadro do jogo
  antes do popup, sem o menu por cima), invisivel para o usuario.
- Na lista de slots: miniatura + "SLOT n" + data/hora; slot vazio aparece como
  "VAZIO". Salvar por cima de um slot usado pede confirmacao.
- Arquivos: por jogo, `estado_N.state` + `estado_N.png` na pasta de dados
  (`/rv-dados/states/<sistema>/<jogo>/`).

## OPCOES > CONVERGENCIA (aprovado)
- Mesmo esquema do RetroVolt do PS2: ajuste de posicao e tamanho da imagem.
- Durante o ajuste, barras laranja nos 4 cantos/bordas da tela mostram onde a
  imagem comeca e termina, para a pessoa ver o que esta mudando.

## OPCOES > PASTAS DE JOGOS: micro SD ou pendrive (aprovado)
- A "base" (jogos, BIOS, saves, configuracao) pode ficar no micro SD ou no pendrive.
- Ao escolher um dispositivo, o RetroVolt cria a estrutura de pastas nele, se
  ainda nao existir (nunca apaga nem sobrescreve nada que ja esteja la).
- Ao trocar de dispositivo, pergunta uma vez: "COPIAR SAVES E CONFIGURACAO PARA O
  NOVO?" (os jogos e as BIOS nao sao copiados).

## Estrutura de pastas (na raiz do dispositivo escolhido)
```
RETROVOLT/
  LEIA-ME.TXT            o que vai em cada pasta
  config/                retrovolt.cfg, controles, ajustes por nucleo e por jogo
  bios/                  BIOS que os nucleos pedem (lista com nomes em LEIA-ME.TXT);
                         OPCOES > INFO mostra quais faltam ou estao erradas (MD5)
  roms/                  uma subpasta por console (nomes abaixo); jogos de varios
                         discos podem ter um .m3u para a troca de disco
  saves/<console>/       saves do proprio jogo (memory card, bateria do cartucho)
  states/<console>/<jogo>/  save states (10 slots) + miniaturas
  tv/                    grades do RetroTV (.txt com link M3U ou .m3u) e cache do guia
  imagens/consoles/      fotos dos consoles para a aba JOGOS (enviadas pelo autor)
  capturas/              fotos de tela (se a pessoa tirar)
  logs/                  LOG.TXT de cada execucao
  versoes/               versoes novas do RetroVolt para testar (so no pendrive)
```
Subpastas de `roms/` (uma por console, criadas so as dos nucleos instalados):
`atari2600, nes, snes, n64, gb, gbc, gba, mastersystem, megadrive, segacd, 32x,
saturn, dreamcast, pcengine, pcenginecd, neogeo, neogeocd, ps1, 3do, arcade`.
A aba JOGOS mostra so as pastas que tem jogos.

## OPCOES > WI-FI (aprovado)
- Buscar redes (lista com nome e forca do sinal), escolher, digitar a senha e pronto.
- Senha: teclado na tela (pelo controle) ou teclado USB, se estiver ligado.
- Mostra a rede conectada e o IP; opcao ESQUECER REDE.
- A senha fica guardada protegida pelo sistema (nunca em texto aberto no pendrive).
- O `retrovolt.cfg` (wifi_nome/wifi_senha) fica so como plano B.

## Caixa RetroFlag 64Pi: botoes POWER e RESET (implementado: caixa.sh)
- Chave SAFE SHUTDOWN da caixa em ON. Nao usa o script da RetroFlag (RPi.GPIO
  nao funciona no Pi 5): overlays gpio-key + config.txt.
- GPIO4 e GPIO14 ligados = Pi funcionando (a caixa corta a energia quando caem).
- POWER (GPIO3 -> KEY_POWER): desligamento seguro pelo systemd-logind.
- RESET (GPIO2 -> KEY_RESTART, servico rv-caixa): toque reinicia o RetroVolt
  (volta ao menu); segurar 5 s reinicia o Pi. Com jogo rodando (futuro): pensar
  se o toque reinicia o jogo.

## R04: leitora de CD + PlayStation (implementado)
- Disco na leitora (menu aberto) = o jogo abre sozinho; ligar o Pi com disco tambem.
  (vai mudar: ver PENDENTE "disco reconhecido -> popup")
  Durante um jogo, trocar o disco so mostra aviso depois.
- JOGOS > PLAYSTATION: "INICIAR CD" e o primeiro item; 2 linhas vazias; depois os jogos.
- rv-jogo (C) roda o nucleo: video no fb com a convergencia (largura/posicao; a altura
  do jogo e 1:1, linha a linha), som ALSA pelo HDMI, START+SELECT = menu do jogo.
- CD virtual: a leitora vira /run/retrovolt/cd/disco.cue+bin pela VFS do libretro
  (READ CD 2352 bytes). Sem comando de velocidade na leitora.
- Save do memory card pelo codigo do jogo (ex.: saves/ps1/SLUS-00892.srm): o mesmo
  save vale para o disco e para a imagem do mesmo jogo.
- EDID ganhou extensao CEA (som PCM + HDMI): precisa rodar atualizar.sh uma vez.

## Aviso de resolucao no jogo (feito no codigo, rv-jogo)
- Ao abrir o nucleo e sempre que a resolucao do jogo muda: "640x480 @ 59.94 Hz" por
  3 s no canto superior direito da imagem (como no ReplayOS).

## (codigo do rv05) disco reconhecido -> popup
- Ao reconhecer um disco NAO iniciar direto: popup no meio da tela com o nome do
  jogo e a plataforma, perguntando INICIAR? (A SIM / B NAO). Nao = volta ao menu.

## LCD (pedido do autor): menu 720p 16:9, jogos 480p, filtros (a fazer)
- Menu no LCD em 1280x720 (16:9): tela logica 426x240 ampliada 3x. Monitor sem 720p:
  o Pi escolhe sozinho o modo da lista do monitor (EDID) mais perto de 720 linhas,
  logo acima ou logo abaixo (ex.: 1366x768, 1280x800, 1024x600), preferindo 16:9; a
  tela logica acompanha a proporcao (240 de altura) e a ampliacao fica inteira.
- Jogos no LCD em 640x480 (480p), proporcao do jogo (4:3) centralizada, laterais pretas.
- OPCOES > VIDEO > FILTRO (so LCD): NENHUM / SCANLINES (intensidade) / CRT (mascara de
  fosforo + brilho leve) / SUAVE. No tubo nunca ha filtro.
- Pre-requisito: deteccao TUBO/LCD em tempo de execucao (hoje a EDID do boot forca 240p):
  ler a EDID real pelo DDC, o RetroVolt escolhe o modo (KMS) - tubo = adaptador AGO/0001.

## Menu do jogo (START+SELECT) - rv05 (codigo)
- Itens: CONTINUAR, VOLUME, OPCOES DO NUCLEO, CONTROLES, REINICIAR JOGO, SAIR DO JOGO.
- OPCOES DO NUCLEO: as opcoes que o proprio nucleo oferece (como no RetroArch), <- -> muda,
  vale na hora; salvas por nucleo em config/nucleos/<nucleo>.opt.
- CONTROLES: cada botao do nucleo (com o nome que o nucleo da: Cross, Circle...) e o botao
  do controle que o aciona; A troca (aperta o botao novo), VOLTAR AO PADRAO; salvo por
  nucleo em config/nucleos/<nucleo>.botoes (PS1, Neo Geo, Sega CD, PC Engine: cada um o seu).
- Avisos de 3 s no canto superior direito: resolucao do jogo a cada troca ("640x480i",
  "320x240p" + Hz) e controle conectado (modelo); texto comprido rola (letreiro).

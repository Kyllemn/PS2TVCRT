# RETROVOLT Pi — decisoes de design (aprovadas pelo autor)

Raspberry Pi 5 + leitora USB (ASUS SDRW-08D1S-U), saida HDMI (LCD ou conversor
HDMI > componente para TV de tubo). Detalhes tecnicos e fases ficam no plano.

## REGRA ABSOLUTA DO AUTOR
- So compilar (rv-jogo, nucleos, qualquer build ou teste compilado) quando o autor
  mandar "#compilar". Nunca antes, em nenhuma hipotese.
- Leitora: manter a leitura a cada 10 s sem uso (nao deixar a leitora dormir).

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

## R06 (feito)
- Menu do jogo: SALVAR ESTADO / CARREGAR ESTADO (10 espacos, miniatura 160x120 PNG do
  ultimo quadro antes do menu, data/hora; substituir pede confirmacao), CONVERGENCIA
  deste nucleo (com o jogo atras; salva em config/nucleos/<nucleo>.conv; SELECT = igual
  ao menu), VOLUME, OPCOES DO NUCLEO, CONTROLES.
- Leitora: durante o jogo, 1 leitura a cada 10 s sem uso (nao entra em descanso).
- Leitora acordando nao conta mais como disco novo (popup so quando o disco sai/entra).
- INFO abre na hora (dados de rede em segundo plano).
- TUBO/LCD automatico: le a EDID verdadeira (kernel no LCD, DDC/i2c no tubo); AGO/0001 =
  tubo; se nao combina com o boot, troca o cmdline.txt e reinicia (max 2 trocas/10 min).
  LCD: menu em 1280x720 (ou o modo mais perto de 720 linhas), tela logica na proporcao
  (16:9 = 426x240) ampliada inteira; jogos 4:3 ampliados inteiros; SCANLINES opcional.
  OPCOES > VIDEO: SAIDA (AUTO/TUBO/LCD), FILTRO. Sem imagem: segurar SELECT 5 s troca.

## PENDENTE: avisos (resolucao/controle) dentro de um balao, mais apresentavel.

## R07 (feito)
- Troca TUBO/LCD so quando o aparelho detectado for diferente do boot atual (compara os
  parametros de video, nao o texto) e com 2 leituras validas e iguais da EDID (soma de
  verificacao), 1 s entre elas.
- Marca maior: "RETRO" (raio amarelo) "VOLT", no menu e no menu do jogo.
- Topo: icone de Wi-Fi + nome da rede (verde conectado). INFO: Wi-Fi, IP, MASCARA, GATEWAY.
- Arte do disco: RETROVOLT/imagens/discos/<sistema>/<codigo>.png (ou nome do arquivo do
  jogo). Aba DISCO e espaco ao lado da lista de jogos; sem arte = CD generico.
- PENDENTE: base de imagens dos discos (procurar repositorio/scraper; ex.: ScreenScraper
  tem "suporte" = arte do disco, precisa conta).

## R08 (feito)
- Logo: raio colado no O e no V.
- Lista de jogos (arquivos): CAPA do jogo a direita (imagens/capas/<sistema>/<codigo>.png
  ou nome do arquivo); INICIAR CD e aba DISCO: arte do CD (imagens/discos/...).
  Reducao numa thread + cache em imagens/.cache (so PNG; webp/jpg nao).
- Receptor 8BitDo ligado desde o boot sem virar controle: 8 s depois do inicio, se o
  receptor (USB 2dc8) estiver la e nenhum controle dele existir, "tira e poe" por
  software (authorized 0 -> 1), uma vez por boot.

## R09: LCD na resolucao nativa
- Teste real (HP w1907, 1440x900): em 1280x720 o monitor deslocou e cortou a imagem.
  Regra nova: LCD usa o modo PREFERIDO (nativo) da EDID; 1280x720 so como reserva.
  1440x900 -> menu 384x240 x3 (1152x720, 16:10, bordas pretas); 1920x1080 -> 427x240 x4.
- Avisos (R09): fundo laranja, letra azul, cantos arredondados. Jogo: resolucao no
  canto superior direito com a SAIDA real ("320x240p 59.94Hz  SAIDA 1440x900" /
  "SAIDA TUBO 240p"); controle conectado no canto inferior esquerdo, mais largo.
  Menu: avisos no canto inferior esquerdo, mesmo estilo.

## R10: filtros no menu do jogo
- FILTRO DE IMAGEM no START+SELECT (so LCD): NENHUM / SCANLINES / SCANLINES LEVES / CRT
  (scanlines + grade de fosforo). Vale na hora e fica salvo por nucleo
  (config/nucleos/<nucleo>.filtro); OPCOES > VIDEO e o padrao. Menu do jogo sem filtro.

## R11 (atualizacao grande; backup antes: commit 94e8f5d / RETROVOLT_BACKUP_R10.zip)
- Disco: identifica em segundo plano (4 s depois do menu); NUNCA abre sozinho (so com A
  na aba DISCO ou INICIAR CD); "saiu" so com 2 leituras seguidas sem disco.
- 8BitDo: o receptor em XInput aparece como Microsoft (045e); religa se nao houver
  controle de 2dc8/045e; lista os USB no log.
- Log com tempos da entrada ("tempo: ...").
- Tela logica adaptativa no LCD: esc = altura//240 (reduz se a largura < 320*esc);
  W = largura//esc, H = altura//esc (1280x720 -> 426x240, 1440x900 -> 480x300,
  800x600 -> 400x300, 640x480 -> 320x240). Tubo continua 320x240.
- Capa e arte do disco no LCD desenhadas com os pontos do monitor (nitidas).
- Menu do jogo em tela cheia, igual ao principal; previa do save state grande
  (miniaturas 320x240); CONVERGENCIA so no tubo (menu e menu do jogo).
- OPCOES > VIDEO > RESOLUCAO (LCD): modos do monitor; aplica e no boot pergunta
  "MANTER?" por 15 s; sem resposta volta sozinho a anterior.

## R12: PC Engine
- Nucleo Beetle PCE Fast (mednafen_pce_fast) para PC Engine (roms/pcengine, .pce) e
  PC Engine CD (roms/pcenginecd, .cue/.chd/.ccd/.m3u; disco fisico pelo CD virtual).
- BIOS obrigatoria do CD: bios/syscard3.pce (o menu avisa se faltar, antes de abrir).
- CD de PC Engine nao tem codigo: save pela "impressao digital" das faixas (PCECD-xxxxxxxx).
- Menu do jogo: IMAGEM (LCD) ORIGINAL (ampliacao inteira) / TELA CHEIA (enche a altura,
  linhas de 3 ou 4 pontos), salvo por nucleo (config/nucleos/<nucleo>.imagem).

## PENDENTE para a proxima release (pedido do autor, fazer junto com outras coisas)
- Save states: 10 espacos POR JOGO (nao por nucleo). Jogo novo = o sistema cria sozinho
  os 10 espacos daquele jogo. Conferir: hoje a pasta e states/<sistema>/<codigo ou nome>;
  achar o caso em que jogos diferentes caem na mesma pasta (ex.: disco sem codigo/rotulo
  vira "DISCO"; jogo aberto antes da R06 sem --states).
- Previa do save state: data e hora EM CIMA da imagem.
- Topo do menu: o nome do Wi-Fi as vezes mostra "SEM WI-FI" estando conectado (o INFO
  mostra certo). Causa provavel: o topo usa o valor da ultima consulta (nmcli a cada 30 s
  e no inicio, antes da rede subir); se a consulta falha/demora, fica vazio. Corrigir:
  consultar com mais frequencia ate achar a rede e nao apagar o nome numa falha isolada.
- Menu do jogo: novo item VOLTAR PARA A LISTA (fecha o jogo e cai na lista de jogos
  daquele sistema, com o cursor no jogo); SAIR DO JOGO continua (volta ao menu principal).
- PC Engine: "INICIAR CD" (leitura direto da leitora) tem que aparecer SEMPRE no topo da
  lista, acima das roms, mesmo sem nenhuma rom de CD. Proposta: uma lista so, PC ENGINE =
  INICIAR CD + 2 linhas + cartoes (.pce) + imagens de CD; nao esconder sistema de CD sem roms.

## R13: correcao da troca de resolucao (bug que deixou o LCD sem imagem)
- A resolucao escolhida so vira definitiva (video_modo na configuracao) depois do
  "MANTER? A". Sem resposta em 15 s: volta o boot anterior e a configuracao continua com a
  ultima confirmada (a deteccao do boot nao forca mais a escolhida sem protecao).
- Sair de VIDEO sem aplicar desfaz a resolucao mexida. Lista: 480 a 1080 linhas.
- Popups de confirmacao (SUBSTITUIR SLOT, DESLIGAR/REINICIAR, COPIAR SAVES, REINICIAR
  AGORA etc.): fundo AZUL (no lugar do vermelho escuro) com contorno BRANCO.
  Bug: no LCD o "SUBSTITUIR O SLOT?" fica atras da previa (a previa nitida e desenhada
  por ultimo direto na tela); a confirmacao tem que ficar por cima.
- Previa do save state com qualidade ruim: a miniatura e tirada pulando pontos (ex.:
  jogo de 640x480 vira 320x240 cortando colunas/linhas, o texto quebra) e depois e
  ampliada 1,5x de forma irregular. Corrigir: gravar a miniatura na resolucao do proprio
  jogo (ate 640x480) e reduzir com media dos pontos so na hora de mostrar.

## PENDENTE (mais tarde): fundo do menu
- Imagem propria: RETROVOLT/imagens/fundo.png vira papel de parede (escurecido para leitura).
- Fundo por console na aba JOGOS (fotos dos consoles enviadas pelo autor).

## R14 (feito)
- LCD em AUTO: o boot nao forca resolucao (o Linux usa a nativa) -> sem reinicio extra.
  So forca acima de 1080 linhas (4K) ou quando escolhida a mao (com confirmacao).
- Wi-Fi do topo: procura a cada 5 s ate achar; so apaga depois de 3 falhas seguidas.
- Save state por jogo: pasta states/<sistema>/<codigo|rotulo|impressao digital do CD|nome>.
- Miniatura na resolucao do jogo (ate 640x480) e reduzida com media; data/hora em cima
  da previa; "SUBSTITUIR?" por cima da previa.
- Popups de confirmacao: fundo azul com contorno branco (menu e menu do jogo).
- Menu do jogo: VOLTAR PARA A LISTA (codigo 10: lista do sistema, cursor no jogo);
  SAIR DO JOGO volta ao menu principal. Itens que nao valem somem (CONVERGENCIA no LCD;
  IMAGEM e FILTRO no tubo).
- PC Engine: lista unica (cartoes + CDs, le tambem roms/pcenginecd), INICIAR CD sempre;
  sistemas de CD com nucleo aparecem sempre; BIOS do CD so e exigida para jogo de CD.

## R15 (feito)
- Leitora sempre girando (regra fixa: a cada 10 s sem leitura, le 1 setor). Agora le
  onde o jogo parou e com o comando certo (audio ou dados). Antes lia dados na faixa 1,
  que no PC Engine e audio: falhava e a leitora dormia (picote/travada ao voltar).
- Leitura adiante: thread le os 6 blocos seguintes enquanto o jogo usa o atual; cache de
  48 blocos; a leitora tem trava propria (o jogo pega do cache sem esperar a leitora).
- Faixa de audio lida em bloco (16 setores num comando), nao setor por setor.
- vsync medido pela mediana de 30 intervalos (uma medida so dava 60,6 Hz e tirava o PC
  Engine do 1 quadro por vsync).
- Log "ritmo" mostra "adiante usados/lidos".

## R16 (compilada)
Sega CD (e Mega Drive / Master System, mesmo nucleo) com Genesis Plus GX.
Licoes do PS1/PCE aplicadas:
- Nucleo le o CD pelo VFS do rv-jogo (cue/bin virtual), com cache, leitura adiante e
  leitora acordada a cada 10 s. "CD Image Cache" do nucleo fica desligado (padrao).
- Leitura: setor que o TOC diz "dados" mas e audio (pausa entre faixas) e lido como
  audio, e vice-versa; antes caia em 3 metodos que falhavam.
- Ao abrir o disco: le 1 setor de musica e volta para dados (a troca de rotacao da
  leitora acontece no carregamento, nao no meio do jogo). Tempos no LOG.
- cue virtual: 8 KB (99 faixas); pausa de 2 s (INDEX 00) so na passagem dados ->
  musica (entre musicas cortaria 2 s do fim de cada musica).
- Ritmo: se o nucleo mudar de Hz no meio do jogo (regiao/modo), recalcula
  "1 quadro por vsync" (antes era calculado so no inicio). MD NTSC 59,92 Hz -> 1:1.
- BIOS do Sega CD pela regiao do disco (codigo de seguranca 0x20B, igual ao nucleo):
  bios_CD_U.bin / bios_CD_E.bin / bios_CD_J.bin em RETROVOLT/bios. Nome com outra
  grafia no cartao: cria o nome exato (o nucleo diferencia maiusculas).
- Memoria interna do Sega CD (saves) em saves/segacd/scd_<regiao>.brm, "por BIOS"
  (como o console: uma memoria para todos os jogos); gravada ao sair do jogo, com
  fsync no pendrive.
- Nome do disco: titulo internacional do cabecalho (0x150), espacos limpos; codigo
  "GM T-xxxx" vira nome do save so se for codigo de verdade.
- Botoes (padrao do nucleo): Y=A, B=B, A=C, L=X, X=Y, R=Z, SELECT=MODE.

- Leitora dormia mesmo com a leitura de 10 s (visto pelo autor no PC Engine): o
  "acorda" lia o ultimo setor do jogo, que ja estava na memoria interna da leitora;
  ela respondia sem girar. Agora le perto, ATRAS da posicao, cada vez num lugar
  diferente (fora da memoria dela). LOG: "leitora acordada (... ms)"; acima de
  300 ms marca "ESTAVA PARANDO".
- Pede a leitora o tempo de descanso maximo (MMC pagina 0x0D, 32 min, sem gravar).
- Leitura que passa de 5 s (leitora acordando) tenta de novo do mesmo jeito e nao
  troca mais o metodo de leitura pelo resto do jogo (antes: 22,7 s e tudo lento).
- Copiar o disco para o cartao: RECUSADO pelo autor (tira a sensacao da midia real).
- Fundo do menu: imagens em RETROVOLT/imagens/wallpapers (qualquer nome .png/.jpg;
  sugerido wallpaper_1 ... wallpaper_10). Sorteia uma a cada inicio, sem repetir a da
  vez anterior (/var/lib/retrovolt/fundo_ultimo). Reduzida para a tela logica do menu
  (pixel art fica nitida; igual no LCD e no tubo), cortando o excesso no centro, e
  escurecida para o texto aparecer (menu_fundo_brilho, padrao 45%). O rv-jogo converte
  (--fundo, stb_image) uma vez; o resultado fica em imagens/.cache. Teste de
  convergencia sempre liso. Disco redondo e furo mostram o fundo (sem quadrado azul).
- Leitora do autor: minimo 10x (o pedido de 4x nao tem efeito nela).
- Nucleos do PS1 e PC Engine mantidos iguais aos da R15 (testados); so o Genesis Plus
  GX e novo. Licao: recompilar nucleo do repositorio muda versao sem querer.


## R17
- CONVERGENCIA do menu voltou (a tela sumiu na rv04 e o menu caia ao abri-la).
- OPCOES > VIDEO: FILTRO so aparece no LCD (no tubo os efeitos nunca sao aplicados);
  monitor que nao informa resolucoes ganha a lista comum (volta automatica protege);
  confirmacao da resolucao nova: 30 s, A ou START (o controle sem fio demora a
  aparecer no boot); "trocas demais" agora aparece na tela; tudo vai para o LOG.
- Efeitos do LCD (no processador, nada a baixar; so em jogos ate 400x300):
  SUAVE (linear 3x), SCALE2X, SCALE3X, XBR (2x, regras do nivel 1). No PC, por
  quadro: nenhum 1,7 ms; SUAVE 5,6; SCALE2X 2,3; SCALE3X 3,4; XBR 3,4.
  Shaders do RetroArch (GLSL) precisam de placa de video: ficam para a fase GPU.
- LOG: "cd: bloco lento" diz onde (lba, musica/dados, faixa) e de onde veio cada travada.
- Tarja laranja esquerda sumida no LCD: o desenho esta certo (conferido em 1280x960);
  e posicao da imagem no monitor (VGA): botao AUTO do monitor.

## R18 — CONVERGENCIA (prioridade do autor)
- Causa do "entrar em convergencia e ele procurar a leitora": a tela de ajuste nao
  existia no codigo desde a rv04 (desenha_conv sumiu). Ao entrar, o menu dava erro e
  caia; o sistema reabria o menu na aba DISCO ("PROCURANDO A LEITORA...").
- Corrigido: a tela voltou (R17); a leitora fica quieta enquanto a convergencia esta
  aberta; erro em qualquer tela agora volta ao menu com aviso e registra no LOG, sem
  derrubar o menu.
- Teste automatico: tools/teste_conv.py (caminho real do controle: OPCOES >
  CONVERGENCIA > A, os 8 botoes, A salva, SELECT padrao, B volta). Na rv16 reproduz o
  erro do LOG; na rv18 passa. Rodar antes de toda versao.

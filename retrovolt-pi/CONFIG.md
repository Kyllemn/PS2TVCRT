# RETROVOLT Pi — arquivo de configuracao (proposta)

Inspirado no `replay.cfg` do ReplayOS: um arquivo de texto simples que o menu
grava e que a pessoa tambem pode abrir e editar no PC para arrumar algo.

## Onde fica
- `RETROVOLT/retrovolt.cfg` no pendrive (facil de editar no PC).
- Copia de seguranca em `/rv-dados/config/` no cartao. Se o do pendrive estiver
  com erro, vale a copia; se nao houver pendrive, vale a do cartao.

## Regras (para nunca travar por causa do arquivo)
- Formato `chave = "valor"`, uma por linha; linhas com `#` sao comentarios.
- O arquivo ja vem com um comentario explicando cada opcao e os valores aceitos.
- Chave desconhecida e ignorada; valor invalido volta para o padrao e o LOG.TXT
  registra qual linha estava errada.
- Apagar o arquivo = voltar tudo ao padrao (o RetroVolt recria na proxima vez).
- Senha do Wi-Fi NAO fica aqui em texto aberto (o replay.cfg faz isso). Para
  configurar Wi-Fi pelo arquivo: preencher `wifi_nome`/`wifi_senha`; no proximo
  boot o RetroVolt conecta, guarda a senha no sistema (protegida) e APAGA a senha
  do arquivo.

## Exemplo (valores padrao)
```
# ---------- VIDEO ----------
video_saida = "auto"          # auto | tubo | lcd
video_tubo_tipo = "15khz"     # 15khz (TV comum)
video_cor_faixa = "auto"      # auto | cheia | limitada  (limitada: adaptadores HDMI>VGA baratos)
video_filtro = "nenhum"       # nenhum | scanlines | crt  (so no LCD)
video_imagem = "original"     # original | tela_cheia
video_hz = "auto"             # auto (50 Hz PAL / 60 Hz NTSC) | 60
video_posicao_x = "0"         # ajuste fino da imagem no tubo (pontos)
video_posicao_y = "0"
video_largura = "1.0"

# ---------- SOM ----------
som_volume = "8"              # 0 a 10

# ---------- CONTROLES ----------
controle_menu = "start+select"
controle_trocar_ab = "nao"    # sim | nao

# ---------- LEITORA ----------
leitora_modo = "ao_vivo"      # ao_vivo | silenciosa | rapida
leitora_janela_mb = "32"      # 8 | 32 | 100
leitora_loading_original = "nao"

# ---------- TV (RetroTV) ----------
tv_pausa_max_min = "3"        # minutos guardados na pausa (Pi de 2 GB: ate 3)

# ---------- SISTEMA ----------
sistema_pasta_jogos = "usb"   # usb | sd
sistema_fuso = "auto"
sistema_log = "normal"        # normal | detalhado

# ---------- WI-FI (preencher so para configurar; a senha e apagada depois) ----------
wifi_nome = ""
wifi_senha = ""
```

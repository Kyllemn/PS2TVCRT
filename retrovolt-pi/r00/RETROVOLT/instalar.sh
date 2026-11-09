#!/bin/bash
# RETROVOLT Pi - instalador (R00). Rodar UMA vez, por SSH, num Raspberry Pi OS Lite
# 64-bit recem-gravado. Nao precisa de tela: o progresso aparece aqui e vai para
# RETROVOLT/LOG_INSTALACAO.TXT no pendrive. So mexe no boot no FINAL, depois de
# conferir tudo; antes disso, se der erro, o Pi OS continua como estava.
set -u
AQUI="$(cd "$(dirname "$0")" && pwd)"
LOG="$AQUI/LOG_INSTALACAO.TXT"
: > "$LOG"
say() { echo "$*"; echo "$(date '+%H:%M:%S') $*" >> "$LOG"; }
falha() { say "ERRO: $*"; say "Nada foi alterado no boot. Mande o arquivo RETROVOLT/LOG_INSTALACAO.TXT."; sync; exit 1; }

say "=== RETROVOLT Pi - instalacao R00 ==="
[ "$(id -u)" = 0 ] || falha "rode com sudo"

# 1. conferencias
modelo=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null)
say "placa: $modelo"
case "$modelo" in *"Raspberry Pi 5"*) ;; *) falha "esta nao e uma Raspberry Pi 5";; esac
[ "$(uname -m)" = aarch64 ] || falha "precisa do Raspberry Pi OS 64-bit"
. /etc/os-release
say "sistema: $PRETTY_NAME"
[ "${VERSION_CODENAME:-}" = trixie ] || falha "precisa do Raspberry Pi OS baseado no Debian 13 (trixie); este e '${VERSION_CODENAME:-?}'. Grave a imagem mais nova no Raspberry Pi Imager."
BOOT=/boot/firmware
[ -f "$BOOT/cmdline.txt" ] && [ -f "$BOOT/config.txt" ] || falha "nao achei $BOOT/cmdline.txt e config.txt"
for f in sistema/rv-lancador sistema/retrovolt.service sistema/retrovolt-crt.bin sistema/initramfs-edid versoes/rv00/rv_app.py; do
	[ -f "$AQUI/$f" ] || falha "arquivo faltando no pendrive: RETROVOLT/$f"
done

# 2. pacotes (precisa de internet: cabo de rede ou Wi-Fi do Imager)
say "baixando a lista de pacotes (precisa de internet)..."
apt-get update >> "$LOG" 2>&1 || falha "sem internet (apt-get update falhou)"
# base de hoje + o que as proximas versoes vao usar (video, som, controles, FFmpeg 7 do RetroTV)
PACOTES="python3 edid-decode exfatprogs dosfstools alsa-utils i2c-tools wireless-tools
 libsdl2-2.0-0 libdrm2 libgbm1 libegl1 libgles2 libasound2t64 libevdev2 libudev1 python3-evdev
 libavcodec61 libavformat61 libavutil59 libswscale8 libswresample5"
faltaram=""
for p in $PACOTES; do
	say "instalando $p"
	DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$p" >> "$LOG" 2>&1 || faltaram="$faltaram $p"
done
[ -n "$faltaram" ] && say "AVISO: nao instalou:$faltaram (a R00 funciona sem eles; anote no log)"
command -v python3 >/dev/null || falha "python3 nao instalou"

# 3. arquivos do RetroVolt
say "copiando o RetroVolt para o cartao"
install -m 755 "$AQUI/sistema/rv-lancador" /usr/local/bin/rv-lancador
install -m 644 "$AQUI/sistema/retrovolt.service" /etc/systemd/system/retrovolt.service
install -d /usr/lib/firmware/edid /opt/retrovolt/versoes /var/lib/retrovolt/logs
install -m 644 "$AQUI/sistema/retrovolt-crt.bin" /usr/lib/firmware/edid/retrovolt-crt.bin
rm -rf /opt/retrovolt/versoes/rv00
cp -r "$AQUI/versoes/rv00" /opt/retrovolt/versoes/rv00
if [ -d /etc/initramfs-tools/hooks ]; then
	install -m 755 "$AQUI/sistema/initramfs-edid" /etc/initramfs-tools/hooks/retrovolt-edid
	say "atualizando o initramfs"
	update-initramfs -u >> "$LOG" 2>&1 || say "AVISO: update-initramfs falhou"
fi

# 4. sistema enxuto: nada de atualizacao automatica no meio do jogo, log so na memoria
say "ajustando o sistema"
for t in apt-daily.timer apt-daily-upgrade.timer man-db.timer e2scrub_all.timer fstrim.timer; do
	systemctl disable --now "$t" >> "$LOG" 2>&1 || true
done
mkdir -p /etc/systemd/journald.conf.d
printf '[Journal]\nStorage=volatile\nRuntimeMaxUse=16M\n' > /etc/systemd/journald.conf.d/retrovolt.conf
systemctl disable getty@tty1.service >> "$LOG" 2>&1 || true
systemctl enable retrovolt.service >> "$LOG" 2>&1 || falha "nao consegui ativar o servico"

# 5. boot (por ultimo): video do tubo (EDID propria, 240p) e boot sem texto/logo
say "ajustando o boot (copia de seguranca em $BOOT/*.antes-retrovolt)"
[ -f "$BOOT/cmdline.txt.antes-retrovolt" ] || cp "$BOOT/cmdline.txt" "$BOOT/cmdline.txt.antes-retrovolt"
[ -f "$BOOT/config.txt.antes-retrovolt" ] || cp "$BOOT/config.txt" "$BOOT/config.txt.antes-retrovolt"
cmd=$(cat "$BOOT/cmdline.txt.antes-retrovolt" | tr -d '\n')
cmd=$(echo "$cmd" | sed -e 's/console=tty1/console=tty3/' -e 's/ quiet//g' -e 's/ splash//g')
cmd="$cmd drm.edid_firmware=HDMI-A-1:edid/retrovolt-crt.bin,HDMI-A-2:edid/retrovolt-crt.bin video=HDMI-A-1:1920x240@60e quiet loglevel=0 logo.nologo vt.global_cursor_default=0 consoleblank=0"
echo "$cmd" > "$BOOT/cmdline.txt"
cp "$BOOT/config.txt.antes-retrovolt" "$BOOT/config.txt"
printf '\n[all]\n# RETROVOLT\ndisable_splash=1\n' >> "$BOOT/config.txt"
say "cmdline: $cmd"
echo "R00 $(date '+%Y-%m-%d %H:%M')" > /var/lib/retrovolt/instalado
sync
say "=== PRONTO. O Pi vai reiniciar em 10 segundos e ligar no RetroVolt (240p). ==="
say "Se a TV ficar sem imagem: por SSH, rode  sudo bash $AQUI/desfazer.sh"
sync
sleep 10
reboot

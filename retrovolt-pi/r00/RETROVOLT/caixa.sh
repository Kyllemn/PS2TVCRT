#!/bin/bash
# RETROVOLT Pi - liga os botoes POWER e RESET da caixa RetroFlag 64Pi (e parecidas).
# Para quem ja instalou o RetroVolt. Rodar por SSH:
#   sudo bash /media/rvusb/RETROVOLT/caixa.sh
# Antes: chave "SAFE SHUTDOWN" dentro da caixa em ON.
#   POWER: desliga o Pi com seguranca (a caixa corta a energia depois).
#   RESET: toque = reinicia o RetroVolt; segurar 5 s = reinicia o Pi.
# Desfazer: desfazer.sh (volta o config.txt de antes do RetroVolt).
AQUI="$(cd "$(dirname "$0")" && pwd)"
BOOT=/boot/firmware
[ "$(id -u)" = 0 ] || { echo "rode com sudo"; exit 1; }
for f in sistema/rv-caixa sistema/rv-caixa.service; do
	[ -f "$AQUI/$f" ] || { echo "faltando: RETROVOLT/$f"; exit 1; }
done
python3 -c 'import evdev' 2>/dev/null || apt-get install -y --no-install-recommends python3-evdev

install -m 755 "$AQUI/sistema/rv-caixa" /usr/local/bin/rv-caixa
install -m 644 "$AQUI/sistema/rv-caixa.service" /etc/systemd/system/rv-caixa.service

# o reset nao reinicia o Pi direto: quem decide e o rv-caixa
mkdir -p /etc/systemd/logind.conf.d
printf '[Login]\nHandlePowerKey=poweroff\nHandleRebootKey=ignore\n' > /etc/systemd/logind.conf.d/retrovolt.conf

if ! grep -q 'RETROVOLT-CAIXA' "$BOOT/config.txt"; then
	cat >> "$BOOT/config.txt" <<'EOF'

[all]
# RETROVOLT-CAIXA (RetroFlag 64Pi): GPIO4 e GPIO14 ligados = Pi funcionando;
# POWER no GPIO3, RESET no GPIO2
gpio=4,14=op,dh
dtoverlay=gpio-key,gpio=3,keycode=116,label="POWER"
dtoverlay=gpio-key,gpio=2,keycode=408,label="RESET"
EOF
	echo "config.txt ajustado"
fi
systemctl daemon-reload
systemctl enable rv-caixa.service
sync
echo "Pronto. Reinicie com: sudo reboot"

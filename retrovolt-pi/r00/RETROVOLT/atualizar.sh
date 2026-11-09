#!/bin/bash
# RETROVOLT Pi - atualiza a parte de sistema (para quem ja instalou). Rodar por SSH:
#   sudo bash /media/rvusb/RETROVOLT/atualizar.sh
# R04: EDID nova com som pelo HDMI (sem ela o Pi manda imagem sem som),
#      lancador, botoes da caixa. Nao mexe no cmdline.txt. Reinicia no final.
AQUI="$(cd "$(dirname "$0")" && pwd)"
[ "$(id -u)" = 0 ] || { echo "rode com sudo"; exit 1; }
LOG="$AQUI/LOG_ATUALIZACAO.TXT"
: > "$LOG"
say() { echo "$*"; echo "$(date '+%H:%M:%S') $*" >> "$LOG"; }
for f in sistema/rv-lancador sistema/retrovolt.service sistema/retrovolt-crt.bin sistema/initramfs-edid caixa.sh; do
	[ -f "$AQUI/$f" ] || { say "faltando: RETROVOLT/$f"; exit 1; }
done
say "copiando lancador e EDID"
install -m 755 "$AQUI/sistema/rv-lancador" /usr/local/bin/rv-lancador
install -m 644 "$AQUI/sistema/retrovolt.service" /etc/systemd/system/retrovolt.service
install -m 644 "$AQUI/sistema/retrovolt-crt.bin" /usr/lib/firmware/edid/retrovolt-crt.bin
if [ -d /etc/initramfs-tools/hooks ]; then
	install -m 755 "$AQUI/sistema/initramfs-edid" /etc/initramfs-tools/hooks/retrovolt-edid
	say "atualizando o initramfs (1 minuto)"
	update-initramfs -u >> "$LOG" 2>&1 || say "AVISO: update-initramfs falhou"
fi
say "botoes da caixa"
bash "$AQUI/caixa.sh" >> "$LOG" 2>&1 || say "AVISO: caixa.sh falhou"
systemctl daemon-reload
echo "R04 $(date '+%Y-%m-%d %H:%M')" >> /var/lib/retrovolt/instalado
sync
say "Pronto. Reiniciando em 5 segundos."
sleep 5
reboot

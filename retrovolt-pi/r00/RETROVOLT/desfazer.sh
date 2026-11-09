#!/bin/bash
# RETROVOLT Pi - desfaz a instalacao: volta o boot como era antes e reativa o
# terminal normal. Rodar por SSH:  sudo bash /media/rvusb/RETROVOLT/desfazer.sh
BOOT=/boot/firmware
[ "$(id -u)" = 0 ] || { echo "rode com sudo"; exit 1; }
for f in cmdline.txt config.txt; do
	if [ -f "$BOOT/$f.antes-retrovolt" ]; then
		cp "$BOOT/$f.antes-retrovolt" "$BOOT/$f" && echo "restaurado $BOOT/$f"
	fi
done
systemctl disable --now retrovolt.service 2>/dev/null
systemctl enable getty@tty1.service 2>/dev/null
rm -f /etc/systemd/journald.conf.d/retrovolt.conf
sync
echo "Pronto. Reinicie com: sudo reboot"

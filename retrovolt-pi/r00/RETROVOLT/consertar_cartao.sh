#!/bin/bash
# RETROVOLT Pi - verifica e conserta o cartao (sistema de arquivos) no proximo boot.
# Use quando aparecer "Bad message" ou erro de leitura em arquivos do sistema
# (costuma vir de desligar sem o desligamento seguro). Rodar por SSH:
#   sudo bash /media/rvusb/RETROVOLT/consertar_cartao.sh
# O Pi reinicia, verifica o cartao (pode levar 1-2 minutos com a tela preta) e
# depois volta ao normal sozinho.
C=/boot/firmware/cmdline.txt
[ "$(id -u)" = 0 ] || { echo "rode com sudo"; exit 1; }
grep -q 'fsck.mode=force' "$C" || sed -i '1 s/$/ fsck.mode=force forcefsck/' "$C"
cat > /etc/systemd/system/rv-fsck-fim.service <<'EOF'
[Unit]
Description=RETROVOLT: tira a verificacao forcada do boot
[Service]
Type=oneshot
ExecStart=/bin/sed -i 's/ fsck.mode=force forcefsck//' /boot/firmware/cmdline.txt
ExecStartPost=/bin/systemctl disable rv-fsck-fim.service
[Install]
WantedBy=multi-user.target
EOF
systemctl enable rv-fsck-fim.service
sync
echo "Reiniciando para verificar o cartao..."
sleep 3
reboot

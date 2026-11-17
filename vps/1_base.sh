#!/bin/bash
# RETROVOLT - VPS, passo 1: base segura (Oracle OCI, Ampere A1 ARM, Ubuntu 22.04).
# Rodar UMA vez na VPS, como o usuario ubuntu:   sudo bash 1_base.sh
# Pode rodar de novo sem estragar nada (cada passo confere antes de mexer).
#
# O que faz:
#   1. atualiza o sistema e liga as atualizacoes de seguranca automaticas
#   2. fuso horario de Sao Paulo, 2 GB de swap (reserva de memoria)
#   3. SSH so por chave (como a Oracle ja entrega) e fail2ban contra tentativas de invasao
#   4. firewall de DENTRO da maquina (iptables da imagem Oracle): abre 80 e 443 (sites)
#      ATENCAO: o firewall do PAINEL da Oracle (Security List) e aberto a mao - ver o LEIA-ME
#   5. instala o Caddy (servidor web com HTTPS automatico) com uma pagina "em construcao"
#   6. cria /srv/retrovolt (onde ficam os sites) e /srv/backup
# Nao mexe em: DNS, e-mail, loja (Yampi). Senhas e chaves NUNCA ficam neste arquivo.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "rode com sudo: sudo bash $0"; exit 1; }
LOG=/var/log/retrovolt-vps.log
exec > >(tee -a "$LOG") 2>&1
echo "=== $(date '+%F %T') passo 1: base ==="

grep -q 'VERSION_ID="22.04"' /etc/os-release || echo "AVISO: feito para Ubuntu 22.04; este e $(. /etc/os-release; echo "$PRETTY_NAME")"
[ "$(uname -m)" = aarch64 ] || echo "AVISO: feito para ARM (Ampere A1); esta maquina e $(uname -m)"

echo "--- 1. atualizacoes"
export DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=a
apt-get update -q
apt-get -y -q -o Dpkg::Options::=--force-confold upgrade
apt-get -y -q install unattended-upgrades fail2ban iptables-persistent curl ca-certificates gnupg \
	debian-keyring debian-archive-keyring apt-transport-https sqlite3 rsync htop
dpkg-reconfigure -f noninteractive unattended-upgrades

echo "--- 2. fuso e swap"
timedatectl set-timezone America/Sao_Paulo
if ! swapon --show | grep -q /swapfile; then
	fallocate -l 2G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile
	grep -q '^/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' >> /etc/fstab
fi

echo "--- 3. SSH so por chave + fail2ban"
cat > /etc/ssh/sshd_config.d/10-retrovolt.conf <<'EOF'
# RETROVOLT: entrar so com chave (a Oracle ja entrega assim); root nunca entra direto
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin no
EOF
sshd -t && systemctl reload ssh
cat > /etc/fail2ban/jail.d/retrovolt.conf <<'EOF'
[sshd]
enabled = true
maxretry = 5
bantime = 1h
EOF
systemctl enable --now fail2ban
systemctl restart fail2ban

echo "--- 4. firewall de dentro da maquina (iptables da imagem Oracle)"
# a imagem da Oracle termina a cadeia INPUT com um REJECT: as regras novas entram ANTES dele
abre() {   # abre <proto> <porta>, logo antes do REJECT (ou no fim, se nao houver REJECT)
	iptables -C INPUT -p "$1" --dport "$2" -m state --state NEW -j ACCEPT 2>/dev/null && return
	local n
	n=$(iptables -L INPUT --line-numbers -n | awk '$2=="REJECT"{print $1; exit}')
	if [ -n "$n" ]; then
		iptables -I INPUT "$n" -p "$1" --dport "$2" -m state --state NEW -j ACCEPT
	else
		iptables -A INPUT -p "$1" --dport "$2" -m state --state NEW -j ACCEPT
	fi
}
abre tcp 80
abre tcp 443
netfilter-persistent save

echo "--- 5. Caddy (HTTPS automatico)"
if ! command -v caddy >/dev/null; then
	curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
	curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' > /etc/apt/sources.list.d/caddy-stable.list
	apt-get update -q
	apt-get -y -q install caddy
fi
mkdir -p /srv/retrovolt/site /srv/backup
if [ ! -f /srv/retrovolt/site/index.html ]; then
	cat > /srv/retrovolt/site/index.html <<'EOF'
<!doctype html><html lang="pt-br"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RetroVolt</title>
<body style="margin:0;min-height:100vh;display:grid;place-items:center;background:#0c1228;color:#f0ece1;font-family:monospace">
<div style="text-align:center"><h1 style="letter-spacing:.2em">RETRO<span style="color:#ff8c28">VOLT</span></h1>
<p>Site novo em construcao.</p><p><a style="color:#ff8c28" href="https://loja.retrovolt.com.br">Ir para a loja</a></p></div>
EOF
fi
if [ ! -f /etc/caddy/Caddyfile.retrovolt ]; then
	# enquanto o dominio ainda aponta para a Yampi, o site responde pelo IP (http, sem cadeado)
	cat > /etc/caddy/Caddyfile <<'EOF'
# RETROVOLT - Caddy. Quando o retrovolt.com.br apontar para esta VPS, o passo 2 troca este
# arquivo e o HTTPS (cadeado) e feito sozinho.
:80 {
	root * /srv/retrovolt/site
	file_server
}
EOF
	touch /etc/caddy/Caddyfile.retrovolt
fi
caddy validate --config /etc/caddy/Caddyfile
systemctl enable --now caddy
systemctl reload caddy

IP=$(curl -s4 --max-time 10 https://ifconfig.me || echo '?')
echo
echo "=== PRONTO (passo 1) ==="
echo "IP publico desta VPS: $IP"
echo "Teste no navegador: http://$IP  (se nao abrir, falta abrir 80/443 no PAINEL da Oracle - ver LEIA-ME)"
echo "Registro deste script: $LOG"

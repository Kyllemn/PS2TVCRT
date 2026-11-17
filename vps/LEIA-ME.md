# RETROVOLT - VPS (Oracle OCI, Ampere A1 ARM, 12 GB, Ubuntu 22.04)

Tudo que roda na VPS e montado por scripts desta pasta, na ordem. Assim da para refazer o
servidor do zero sem depender de memoria. **Nenhuma senha ou chave fica aqui** (este
repositorio e publico).

## Plano

| Passo | O que | Situacao |
|---|---|---|
| 1 | Base segura: atualizacoes, firewall, SSH so por chave, fail2ban, Caddy (HTTPS) | `1_base.sh` pronto |
| 2 | Loja Yampi em `loja.retrovolt.com.br` e o `retrovolt.com.br` apontando para a VPS | depois do passo 1 |
| 3 | Site do RetroVolt Pi + cadastro + jogatina online (salas) | a fazer |
| 4 | Backups automaticos para fora da VPS (Object Storage da Oracle) | a fazer |
| 5 | E-mail proprio (receber na VPS, enviar pelo Email Delivery da Oracle) | a fazer |
| 6 | Servidor do jogo online (saguao e retransmissao) | quando o rv-jogo tiver netplay |

## Passo 1: base

### 1a. Abrir as portas no PAINEL da Oracle (so uma vez)
A Oracle tem um firewall no painel, alem do de dentro da maquina (o script cuida desse).
1. Console da Oracle > **Networking > Virtual Cloud Networks** > a sua VCN.
2. **Security Lists** > **Default Security List** > **Add Ingress Rules**.
3. Adicione duas regras (uma de cada vez):
   - Source CIDR `0.0.0.0/0`, IP Protocol **TCP**, Destination Port Range **80**
   - Source CIDR `0.0.0.0/0`, IP Protocol **TCP**, Destination Port Range **443**
4. Nao mexa na regra da porta **22** (e por ela que voce entra pelo SSH).

### 1b. Entrar na VPS e rodar o script
Do seu PC (PowerShell ou Terminal), com a chave que a Oracle deu ao criar a maquina:
```
ssh -i caminho\da\chave.key ubuntu@IP_DA_VPS
```
Ja dentro da VPS:
```
curl -fsSL https://raw.githubusercontent.com/kyllemn/ps2tvcrt/claude/new-session-508stw/vps/1_base.sh -o 1_base.sh
sudo bash 1_base.sh
```
Leva uns 5 a 10 minutos. No fim ele mostra o IP e o endereco para testar.

### 1c. Conferir
- Abra `http://IP_DA_VPS` no navegador: deve aparecer "RETROVOLT - Site novo em construcao".
- Mande para o Claude as ultimas linhas que o script mostrou (ou o arquivo
  `/var/log/retrovolt-vps.log`). Nao tem senha nele.

### Se algo der errado
- **Nao abre no navegador:** quase sempre falta a regra 80/443 do passo 1a.
- **Perdeu o acesso SSH:** o script so desliga senha (a Oracle ja entra por chave). Pelo painel
  da Oracle da para abrir o **Console Connection** da maquina.

## Cuidados com a Oracle
- **Conta "Always Free" pode ter a maquina recolhida** se ficar parada (CPU baixa por dias).
  Recomendado: mudar a conta para **Pay As You Go** (continua sem custo dentro dos limites gratis).
- **Envio de e-mail pela porta 25 e bloqueado** pela Oracle: o passo 5 usa o Email Delivery dela.
- **Backup fora da maquina e obrigatorio** (passo 4).

## Loja (Yampi) - fica na Yampi
A loja continua hospedada na Yampi (checkout, pagamento e seguranca de cartao sao deles).
No passo 2 ela passa a responder em `loja.retrovolt.com.br`; so depois disso o
`retrovolt.com.br` vai para a VPS, para a loja nunca sair do ar.

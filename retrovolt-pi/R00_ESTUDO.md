# R00 — Base do sistema (estudo para aprovacao)

Objetivo: gravar o cartao UMA vez e, dai pra frente, testar cada versao nova
copiando arquivos no pendrive, sem reinstalar nada. Nenhum codigo de jogo
nesta fase: so a "fundacao".

## 1. O que o autor faz (uma vez)
1. No PC, Raspberry Pi Imager > Raspberry Pi 5 > **Raspberry Pi OS Lite (64-bit)**.
   Nas configuracoes do Imager: usuario `rv`, senha a escolher, Wi-Fi (opcional),
   SSH ligado (ajuda no diagnostico; pode desligar depois).
2. Copia a pasta `RETROVOLT/` (que eu mando, alguns MB) para a raiz de um
   pendrive FAT32/exFAT.
3. Liga o Pi com o pendrive. Na primeira vez, entra no terminal do Pi OS e roda
   uma linha: `sudo sh /media/.../RETROVOLT/instalar.sh` (ou o instalador roda
   sozinho, se der pra automatizar com seguranca — decidir no teste).
4. O Pi reinicia ja no RetroVolt (no R00/R01: a tela de teste).

## 2. O que o instalador faz
- Confere se e Pi 5 + Pi OS 64-bit (senao, para e explica na tela).
- Instala so o necessario (bibliotecas de video KMS/DRM, audio ALSA, USB, rede).
- Tira o que nao serve (servicos de area de trabalho, bluetooth se nao usado,
  atualizacoes automaticas que poderiam rodar no meio do jogo, etc.).
- Cria a particao/pasta de DADOS (`/rv-dados`): configuracoes, saves, logs.
- Instala o **lancador** e um servico do sistema (systemd) que o abre logo no
  boot, direto no video (sem login, sem area de trabalho, sem texto de boot).
- Ajusta o boot: sem logo/texto do Linux, video ja no modo certo (R01/R02).
- Liga a **protecao do cartao** (sistema somente-leitura com "overlay"):
  desligar da tomada nao corrompe o sistema. So `/rv-dados` grava, e de forma
  segura (escreve e sincroniza na hora).
- Botao de liga/desliga da placa do Pi 5: aperto curto = desligar com seguranca.

## 3. O lancador (o coracao do esquema de testes)
Ordem ao ligar:
1. Procura pendrive com `RETROVOLT/`.
2. Se houver uma versao nova la (`RETROVOLT/versoes/rvNN/`), usa ela.
   Senao usa a ultima versao instalada no cartao.
3. **Volta automatica:** conta tentativas de boot. Se uma versao travar ou cair
   2 vezes seguidas sem chegar na tela inicial, o lancador marca como ruim e
   abre a ultima que funcionou, mostrando um aviso.
4. Opcional: "instalar esta versao no cartao" (para usar sem o pendrive).
5. Atalho de emergencia: segurar SELECT (ou um botao do teclado) ao ligar =
   modo seguro (video seguro + versao do cartao).

## 4. Logs para diagnostico
- Cada execucao grava `RETROVOLT/LOG.TXT` no pendrive (e em `/rv-dados/logs`).
- Conteudo: versao, modelo/memoria do Pi, temperatura, telas/EDID detectadas,
  leitora detectada, erros. Sem nada pessoal (sem senha de Wi-Fi etc.).
- O autor me manda o LOG.TXT, como nos testes do CDUSB.

## 5. O que fica pronto ao fim do R00 (junto com o R01)
- Pi liga direto numa tela nossa (a tela de teste de video do R01).
- Versoes novas por pendrive; volta automatica; log.
- Desligar pelo botao ou pela tomada sem estragar o cartao.

## 6. Riscos e como tratar
| Risco | Tratamento |
|---|---|
| Instalador falhar no meio | ele so muda coisas no fim, depois de conferir tudo; se falhar, o Pi OS continua normal e da para rodar de novo |
| Tela preta (modo de video errado) | modo seguro ao segurar SELECT; o R00 usa um modo de video conservador ate o R01 aprovar os outros |
| Pendrive nao montar | o lancador cai na versao do cartao e registra no log |
| Versao nova travar | volta automatica (item 3) |
| Atualizacao do Pi OS mudar algo | atualizacoes automaticas desligadas; atualizar so quando decidirmos |

## 7. Decisoes ja tomadas
- Caminho A agora (Pi OS Lite + instalador); imagem propria (B) so no final.
- Pi 5 com 2 GB: sistema o mais enxuto possivel.

## 8. Perguntas para o autor antes de comecar o codigo
1. Usuario `rv` esta bom? Quer Wi-Fi ja configurado pelo Imager?
2. Tem teclado USB para a primeira vez (rodar o instalador)?
3. O pendrive de testes sera o mesmo dos jogos, ou um so para o RETROVOLT?

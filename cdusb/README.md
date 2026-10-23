# CDUSB

Leitor de CD USB externo no PS2, para os emuladores de PC Engine CD, Sega CD e Neo Geo CD lerem o disco direto do leitor.

Fase 1 (esta): driver `cdusb.irx` + `CDUSB.ELF`, que testa o leitor e grava o resultado em `mass:/CDUSB.TXT`.

## Como testar

1. Leitor no PS2 pelo cabo Y: plugue principal numa porta do PS2, plugue de energia num carregador de celular.
2. Pendrive na outra porta (o log vai para ele).
3. Ligue o leitor **antes** de abrir o `CDUSB.ELF`.
4. Abra o `CDUSB.ELF` pelo wLaunchELF e coloque um disco com dados e música (PC Engine CD, Sega CD ou Neo Geo CD).
5. No fim: X repete, O sai. Mande o `CDUSB.TXT`.

## Peças

| Pasta | O que é |
|---|---|
| `iop/` | `cdusb.irx`: fica com os leitores de CD USB e transporta os comandos SCSI/MMC. Os pendrives ficam com o `usbmass_bd`. |
| `ee/` | `CDUSB.ELF`: diagnóstico (TOC, leitura de dados e música, subcanal, velocidade, busca, motor). `cdusb_ee.c` é o cliente que os emuladores vão usar. |
| `common/` | `cdusb_rpc.h`: conversa entre IOP e EE. |

## Compilar

```
/opt/ps2/ps2.sh /caminho/cdusb "make"
```

Precisa do ambiente do `preparar_ps2.sh` (imagem `ps2dev/ps2dev` + make).

# PCEUSB

PC Engine CD rodando **direto do leitor de CD USB** no PS2.

- Núcleo: o Mednafen PCE Fast otimizado para PS2 (o mesmo do RETROVOLT), em `core/`.
- Disco: `cdusb.irx` + `cdcache` (de `../cdusb`): leitor travado em 10x, leitura antecipada numa thread, manter girando, busca do jogo avisada ao leitor (`cdcd_seek`).
- Vídeo: gsKit, 240p, textura de 16 bits no formato do GS (paleta do núcleo para PS2), sem filtro.
- Som: thread própria alimentando o audsrv sem parar.
- Controle: ○ = I, ✕ = II, SELECT, START = RUN. **SELECT + START** (segurar 1 s) sai.

A System Card vai **embutida** no ELF a partir de `res/syscard3.pce`, que fica fora do git. Por isso o `PCEUSB.ELF` é só para uso pessoal e não deve ser divulgado.

## Compilar

```
/opt/ps2/ps2.sh /caminho/do/repositorio "make -C cdusb/iop && make -C pceusb"
```

# fm-loader

Menu de boot de PS1 para um CD com Front Mission 1st + Front Mission 2.

- Tela 640x480 entrelaçada, feita a partir do mockup (`mockup/mock.py`): fundo, título, capas
  grandes e o disco do jogo selecionado girando.
- `←`/`→` escolhe, `✕`/`○`/`START` inicia. O menu aplica a configuração do kernel do jogo
  (TCB/EVENT/STACK do SYSTEM.CNF dele) e chama o `LoadExec` da BIOS.
- Os dois jogos acham seus arquivos pelo nome a partir da raiz e os nomes não colidem,
  então os arquivos dos dois ficam juntos na raiz do CD sem patch.

Montagem: `./build.sh` (precisa do PSn00bSDK em `/opt/psx/sdk`), depois
`python3 tools/build_combo.py fm1.bin fm2.bin saida/`.

Imagens (capas, discos, fundo) e arquivos dos jogos não vão para o git.

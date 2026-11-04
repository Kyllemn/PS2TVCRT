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

## Formato novo (2 a 4 jogos)
- `tools/menu_dat.py config.json MENU.DAT`: gera o MENU.DAT 'MNU2' com a lista de jogos (executavel,
  TCB/EVENT/STACK), tela base e so os retangulos que mudam por jogo (cabe na RAM com 3-4 jogos).
- `tools/cdsector.py`: setores MODE2 (EDC/ECC), leitura de .bin e listagem ISO9660.
- `tools/build_ffcol.py`: CD Final Fantasy (Origins + FF III NES + FF IV). Raiz e path tables nos
  setores 18-20; FF III (imbNES) logo depois, como na ISO dele; menu; FF IV (26 posicoes MSF
  corrigidas no SLUS_013.60); Origins (`--ff2-lba`: posicao do DATA.BIN e as 1548 posicoes do
  indice dele). Licenca japonesa (da ISO do FF III): com a americana o leitor de CD proprio do
  imbNES trava.
- Desenho do disco pela tabela de ordenacao (DrawOTag): o DrawPrim direto travava no Beetle.

# RetroLoader

RetroLoader é um launcher de ISOs e apps para PS2, feito para atender às demandas de TVs CRT no mercado. Letras maiores, fácil navegação, para pegar e jogar. Execute seus jogos diretamente da raiz de qualquer dispositivo. Navegue fácil pelos menus. Bem-vindo ao mundo CRT dos anos 2000, a era de ouro do PS2. Usa o Neutrino para o boot/chainload dos jogos.

## Funcionalidades

- **Favoritos**: marque seus jogos mais jogados e acesse direto sem navegar pela lista completa — algo que o OPL não oferece
- **Confirmação de boot**: popup de confirmação antes de iniciar um jogo, inclusive pela tela de favoritos, evitando boots acidentais
- **Modo de compatibilidade por jogo**: fixe o modo de compatibilidade de cada título individualmente, sem precisar reconfigurar toda vez
- **Suporte a múltiplos dispositivos**: HD, USB, MX4SIO e MMCE (dois slots), com detecção automática do que está conectado
- **Boot direto do memory card**: também é possível rodar a partir do mc0:, sem depender só de HD/USB
- **Interface própria**, sem depender de temas ou skins de terceiros
- **Assets embutidos no próprio `.elf`**: nada de arquivos soltos de imagem/ícone espalhados pelo dispositivo — um único arquivo já traz tudo

## Instalação

1. Copie `RetroLoader.elf` para a raiz do seu dispositivo de armazenamento (USB/HD/MX4SIO/memory card).
2. Garanta que o Neutrino esteja instalado junto, na seguinte estrutura:

```
/
├── RetroLoader.elf
└── FastLoader/
    ├── neutrino.elf
    ├── config/
    └── modules/
```

3. Inicie o `RetroLoader.elf` pelo seu método de boot habitual (OPL, browser, etc), ou renomeie para `BOOT.ELF` e substitua de vez os boots do seu PS2. Dentro dele você acessa e inicia seus arquivos `.ELF` e `.ISO` diretamente.

## Notas

- O código-fonte deste projeto é fechado; apenas builds compilados são publicados aqui, pelas Releases.
- O Neutrino é um projeto open-source independente — consulte o [repositório oficial dele](https://github.com/rickgaiser/neutrino) para mais detalhes.

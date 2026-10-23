# Notas guardadas (gatilhos)

## #pce

Lembrar quando o usuário invocar **#pce**:

- **No disco de PC Engine CD, a faixa 2 é de dados.** A faixa 1 é de música (o aviso "este disco é para PC Engine").
- Por isso, **os últimos setores da faixa 1 geram erros de leitura**: os 150 setores (2 s) antes do início da faixa 2 são a pausa (pregap) da faixa de dados. Não são música, e o leitor recusa lê-los com o comando de música (READ CD tipo CD-DA).
- Visto no teste da c03 (disco de PC Engine, 22 faixas: faixa 1 música no setor 0, faixa 2 dados no setor 3890): 10 erros na passagem da faixa 1 para a 2, com chiado e glitch.
- Tratamento feito na c04 (`cdusb/ee/cdcache.c`): numa faixa de música seguida de faixa de dados, os últimos 150 setores viram silêncio sem ler. O mesmo vale ao contrário (faixa de dados seguida de música: os 150 setores antes da música não são lidos em bloco).

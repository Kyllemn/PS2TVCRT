#!/bin/sh
# Monta a traducao completa do disco 1. uso: build.sh SCRATCH  (com bm_files/, bm_tools/, trad/, bm_dl/arquivo.bin)
set -e
S=$1; T=$S/bm_tools; B=$S/build; mkdir -p $B; echo '{}' > $B/mapa.json
python3 -I $T/junta_roteiro.py $S/trad $T/roteiro_jp.tsv $B/roteiro_pt.tsv
python3 -I $T/insere_exe.py $S/bm_files/1ST_READ.BIN $T/exe_pt.tsv $B/mapa.json $T $B/1ST_READ.BIN
python3 -I $T/insere_param.py $S/bm_files/PARAMETA.DAT $T $T/param_jp.tsv $S/trad $B/mapa.json $B/PARAMETA.DAT
python3 -I $T/nome_sys.py $T $S/bm_files/SYSTEM.DAT $B/mapa.json $B/SYSTEM_nome.DAT
python3 -I $T/insere3.py $S/bm_files/SCENARIO.DAT $B/SYSTEM_nome.DAT $T/psi_spec.json $B/roteiro_pt.tsv $T $B/SCENARIO.DAT $B/SYSTEM_fonte.DAT $B/mapa.json
python3 -I $T/img_sys.py $T $B/SYSTEM_fonte.DAT $B/SYSTEM.DAT
python3 -I $T/img_hud.py $T $S/bm_files/EVENT.DAT $B/EVENT.DAT
python3 -I $T/grava_track.py $S/bm_dl/arquivo.bin $T/lista.txt $B/track03.bin SCENARIO.DAT=$B/SCENARIO.DAT SYSTEM.DAT=$B/SYSTEM.DAT 1ST_READ.BIN=$B/1ST_READ.BIN PARAMETA.DAT=$B/PARAMETA.DAT EVENT.DAT=$B/EVENT.DAT

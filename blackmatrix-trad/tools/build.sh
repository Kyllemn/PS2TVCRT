#!/bin/sh
# Monta a traducao completa do disco 1. uso: build.sh SCRATCH  (com bm_files/, bm_tools/, trad/, bm_dl/arquivo.bin)
set -e
S=$1; T=$S/bm_tools; B=$S/build; F=$S/bm_files; mkdir -p $B; echo '{}' > $B/mapa.json
p() { python3 -I "$@"; }
# textos
p $T/junta_roteiro.py $S/trad $T/roteiro_jp.tsv $B/roteiro_pt.tsv
p $T/insere_exe.py $F/1ST_READ.BIN $T/exe_pt.tsv $B/mapa.json $T $B/1ST_READ.BIN
p $T/insere_param.py $F/PARAMETA.DAT $T $T/param_jp.tsv $S/trad $B/mapa.json $B/PARAMETA_t.DAT
p $T/nome_sys.py $T $F/SYSTEM.DAT $B/mapa.json $B/SYS1.DAT
p $T/insere3.py $F/SCENARIO.DAT $B/SYS1.DAT $T/psi_spec.json $B/roteiro_pt.tsv $T $B/SCENARIO.DAT $B/SYS2.DAT $B/mapa.json
# imagens
p $T/img_sys.py $T $B/SYS2.DAT $B/SYS3.DAT
p $T/img_menu.py $T $B/SYS3.DAT $B/SYS4.DAT
p $T/img_cap.py $T $B/SYS4.DAT $B/SYS5.DAT
p $T/img_escolha.py $T $B/SYS5.DAT $B/SYS6.DAT
p $T/img_disco.py $T $B/SYS6.DAT $B/SYSTEM.DAT
p $T/img_rel.py $T $B/PARAMETA_t.DAT $B/PARAMETA.DAT
p $T/img_hud.py $T $F/EVENT.DAT $B/EV1.DAT
p $T/img_lugar.py $T $B/EV1.DAT $B/EV2.DAT
p $T/img_scl.py $T $B/EV2.DAT $B/EVENT.DAT
p $T/img_bat.py $T $F/CHAR.DAT $B/CHAR.DAT
rm -f $B/SYS?.DAT $B/EV?.DAT $B/PARAMETA_t.DAT $B/*.png
p $T/grava_track.py $S/bm_dl/arquivo.bin $T/lista.txt $B/track03.bin SCENARIO.DAT=$B/SCENARIO.DAT SYSTEM.DAT=$B/SYSTEM.DAT 1ST_READ.BIN=$B/1ST_READ.BIN PARAMETA.DAT=$B/PARAMETA.DAT EVENT.DAT=$B/EVENT.DAT CHAR.DAT=$B/CHAR.DAT

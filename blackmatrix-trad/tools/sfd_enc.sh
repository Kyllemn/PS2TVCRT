#!/bin/sh
# uso: sfd_enc.sh entrada.SFD saida.m1v fps kbps tmpdir
# decodifica para YUV cru (mesma contagem de quadros) e reencoda em 2 passadas
I=$1; O=$2; R=$3; B=$4; T=$5
ffmpeg -v error -y -f mpeg -i "$I" -map 0:v -fps_mode passthrough -f rawvideo -pix_fmt yuv420p "$T/q.yuv" || exit 1
C="-c:v mpeg1video -b:v ${B}k -maxrate 3600k -minrate 0 -bufsize 1835008 -g 15 -bf 2 -sc_threshold 1000000000 -mbd rd -trellis 2 -cmp 2 -subcmp 2 -qmin 2"
E="-f rawvideo -pix_fmt yuv420p -s 320x240 -framerate $R -i $T/q.yuv"
ffmpeg -v error -y $E $C -pass 1 -passlogfile "$T/p" -f mpeg1video /dev/null &&
ffmpeg -v error -y $E $C -pass 2 -passlogfile "$T/p" -f mpeg1video "$O"
r=$?; rm -f "$T/q.yuv" "$T"/p*; exit $r

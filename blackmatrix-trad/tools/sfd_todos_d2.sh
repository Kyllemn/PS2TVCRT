#!/bin/sh
# reencoda e remonta os SFD do disco 2: d2_sfd/orig -> d2_sfd/novo
cd "$(dirname "$0")"; K=${1:-1500}
for f in d2_sfd/orig/*.SFD; do
  n=$(basename $f .SFD)
  fr=$(python3 -c "d=open('$f','rb').read(); i=d.find(b'\x00\x00\x01\xb3'); print({2:24,5:30,3:25}[d[i+7]&15])")
  ./sfd_enc.sh $f d2_sfd/m1v/$n.m1v $fr $K d2_sfd/log 2>/dev/null || { echo FALHOU $n; exit 1; }
  python3 sfd_mux.py $f d2_sfd/m1v/$n.m1v d2_sfd/novo/$n.SFD || exit 1
done
echo TUDO_OK

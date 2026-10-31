#!/bin/sh
# reencoda e remonta todos os SFD de sfd/orig -> sfd/novo
cd "$(dirname "$0")"; K=${1:-1000}
for f in sfd/orig/*.SFD; do
  n=$(basename $f .SFD)
  fr=$(python3 -c "d=open('$f','rb').read(); i=d.find(b'\x00\x00\x01\xb3'); print({2:24,5:30,3:25}[d[i+7]&15])")
  ./sfd_enc.sh $f sfd/m1v/$n.m1v $fr $K sfd/log 2>/dev/null || { echo FALHOU $n; exit 1; }
  python3 sfd_mux.py $f sfd/m1v/$n.m1v sfd/novo/$n.SFD || exit 1
done
echo TUDO_OK

#!/bin/bash
# mostra um bloco sem os rotulos de nome repetidos; JP so quando difere bastante
python3 /root/fm2/dump.py $1 $2 | grep -v -P "\t\{(Ven|Gwianda|Henschel|Hordman|Domingo|Saribash|Azad|Chang-Jang):\}$" | grep -v -P "^\t\tJP\t(ヴェン|グィアンダ|ヘンシェル|ホードマン|ドミンゴ|サリバシュ|アザド|チャンジャン)：$" | cut -c1-210

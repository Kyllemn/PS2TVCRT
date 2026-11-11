#!/usr/bin/env python3
"""YabaSanshiro: o disco (cue/bin/iso) passa a ser lido pelo VFS do libretro, como nos outros
nucleos de CD. Sem isso ele abre o arquivo com fopen e nao enxerga o CD virtual do rv-jogo
(/run/retrovolt/cd/disco.cue, montado da leitora). A linha ja existia no cdbase.c, comentada
por causa do Windows; aqui e Linux.  uso: yaba_vfs.py pasta_do_clone"""
import sys, os
raiz = os.path.join(sys.argv[1], 'yabause', 'src')

p = os.path.join(raiz, 'cdbase.c')
s = open(p, newline='').read()
a = '// #include "streams/file_stream_transforms.h"'
if a in s:
    s = s.replace(a, '#include "streams/file_stream_transforms.h"')
    open(p, 'w', newline='').write(s)
elif '#include "streams/file_stream_transforms.h"' not in s:
    sys.exit('yaba_vfs: cdbase.c mudou, remendo nao aplicado')

p = os.path.join(raiz, 'libretro', 'libretro.c')
s = open(p, newline='').read()
if 'filestream_vfs_init' not in s:
    nl = '\r\n' if '\r\n' in s else '\n'
    a = 'void retro_set_environment(retro_environment_t cb)' + nl + '{' + nl
    if a not in s:
        sys.exit('yaba_vfs: libretro.c mudou, remendo nao aplicado')
    s = s.replace(a, a + '''   {  /* RETROVOLT: disco pelo VFS do frontend (CD virtual da leitora) */
      struct retro_vfs_interface_info vfs = { 1, NULL };
      if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs))
         filestream_vfs_init(&vfs);
   }
'''.replace('\n', nl), 1)
    s = s.replace('#include <file/file_path.h>' + nl, '#include <file/file_path.h>' + nl + '#include <streams/file_stream.h>' + nl, 1)
    open(p, 'w', newline='').write(s)
print('yaba_vfs: ok')

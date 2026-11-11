#!/usr/bin/env python3
"""Beetle Saturn: liga o VFS do frontend (filestream_vfs_init). O nucleo ja le tudo por
filestream, mas sem o init usa o arquivo real e nao ve o CD virtual da leitora.
uso: beetle_saturn_vfs.py pasta_do_clone"""
import sys, os
p = os.path.join(sys.argv[1], 'libretro.c')
s = open(p, newline='').read()
if 'filestream_vfs_init' not in s:
    a = 'void retro_set_environment(retro_environment_t cb)\n{\n'
    if a not in s:
        sys.exit('beetle_saturn_vfs: libretro.c mudou, remendo nao aplicado')
    s = s.replace(a, a + '''   {  /* RETROVOLT: disco pelo VFS do frontend (CD virtual da leitora) */
      struct retro_vfs_interface_info vfs = { 3, NULL };
      if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs))
         filestream_vfs_init(&vfs);
   }
''', 1)
    s = '#include <streams/file_stream.h>\n' + s
    open(p, 'w', newline='').write(s)
print('beetle_saturn_vfs: ok')

/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <streams/file_stream.h>

#include "file.h"
#include "mednafen-endian.h"

#if defined(PS2) || defined(XENON360)
#include <strings.h>
/* PS2 (m13): System Card 3.0 embutida no ELF (res/syscard3.pce) */
extern const unsigned int ps2_syscard3_size;
extern const unsigned char ps2_syscard3[];
#endif

struct MDFNFILE *file_open(const char *path)
{
   int64_t size          = 0;
   const char        *ld = NULL;
   struct MDFNFILE *file = (struct MDFNFILE*)calloc(1, sizeof(*file));

   if (!file)
      return NULL;

#if defined(PS2) || defined(XENON360)
   {
      const char *base = strrchr(path, '/');
      base = base ? base + 1 : path;
      if (!strcasecmp(base, "syscard3.pce"))
      {
         file->data = (uint8_t*)malloc(ps2_syscard3_size);
         if (!file->data)
            goto error;
         memcpy(file->data, ps2_syscard3, ps2_syscard3_size);
         file->size = ps2_syscard3_size;
         file->ext  = strdup("pce");
         return file;
      }
   }
#endif

   if (!filestream_read_file(path, (void**)&file->data, &size))
      goto error;

   ld         = (const char*)strrchr(path, '.');
   file->size = size;
   file->ext  = strdup(ld ? ld + 1 : "");

   return file;

error:
   if (file)
      free(file);
   return NULL;
}

int file_close(struct MDFNFILE *file)
{
   if (!file)
      return 0;

   if (file->ext)
      free(file->ext);
   file->ext = NULL;

   if (file->data)
      free(file->data);
   file->data = NULL;

   free(file);

   return 1;
}

# PCEUSB.ELF - PC Engine CD pelo leitor de CD USB (PS2). Criado por Leonardo Torres.
# Compilar: /opt/ps2/ps2.sh <pasta do repositorio> "make -C cdusb/iop && make -C pceusb"
# A System Card fica em res/syscard3.pce (fora do git) e vai embutida no ELF.
EE_BIN = PCEUSB.ELF

C := core
M := $(C)/mednafen
P := $(M)/pce_fast
LC := $(C)/libretro-common
D := $(M)/cdrom
CDUSB := ../cdusb
IRXDIR = $(PS2SDK)/iop/irx
BIN2C = $(PS2SDK)/bin/bin2c

CORE_SRCS = \
	$(M)/hw_misc/arcade_card/arcade_card.c \
	$(P)/pcecd_drive.c $(P)/pcecd.c $(P)/psg.c $(P)/huc6280.c $(P)/input.c $(P)/vdc.c \
	$(M)/sound/Blip_Buffer.c \
	$(D)/CDAccess.c $(D)/CDAccess_Image.c $(D)/CDAccess_CCD.c $(D)/audioreader.c \
	$(D)/cdromif.c $(D)/CDUtility.c $(D)/lec.c $(D)/galois.c $(D)/l-ec.c \
	$(D)/edc_crc32.c $(D)/recover-raw.c \
	$(C)/libretro.c $(M)/general.c $(M)/cdstream.c $(M)/mempatcher.c $(M)/okiadpcm.c \
	$(M)/file.c $(M)/settings.c $(M)/state.c $(M)/mednafen-endian.c \
	$(filter-out %ivorbisfile_example.c, $(wildcard $(M)/tremor/*.c)) \
	$(LC)/streams/file_stream.c $(LC)/streams/file_stream_transforms.c \
	$(LC)/file/file_path.c $(LC)/file/retro_dirent.c $(LC)/lists/string_list.c \
	$(LC)/lists/dir_list.c $(LC)/compat/compat_strl.c $(LC)/compat/compat_snprintf.c \
	$(LC)/compat/compat_posix_string.c $(LC)/compat/compat_strcasestr.c \
	$(LC)/compat/fopen_utf8.c $(LC)/encodings/encoding_utf.c $(LC)/encodings/encoding_crc32.c \
	$(LC)/memmap/memalign.c $(LC)/string/stdstring.c $(LC)/time/rtime.c \
	$(LC)/vfs/vfs_implementation.c

IRX_NAMES = iomanx filexio usbd bdm bdmfs_fatfs usbmass_bd audsrv cdusb
GEN_OBJS = build/gen/syscard.o $(patsubst %,build/gen/irx_%.o,$(IRX_NAMES))
EE_OBJS = $(patsubst %.c,build/%.o,$(CORE_SRCS)) build/src/main.o build/src/ps2_cd.o build/src/ui.o \
	  build/cdusb/cdusb_ee.o build/cdusb/cdcache.o $(GEN_OBJS)

EE_INCS := -I$(C) -I$(M) -I$(M)/include -I$(M)/hw_sound -I$(M)/hw_cpu -I$(M)/hw_misc \
	   -I$(LC)/include -Isrc -I$(CDUSB)/common -I$(CDUSB)/ee -I$(GSKIT)/include
CORE_FLAGS = -DPS2 -DMEDNAFEN_VERSION_NUMERIC=931 -DWANT_PCE_FAST_EMU -DSTDC_HEADERS \
	     -D__STDC_LIMIT_MACROS -D__LIBRETRO__ -D_LOW_ACCURACY_ -DINLINE=inline \
	     -DNEED_CD -DNEED_TREMOR -DNDEBUG
EE_CFLAGS := $(CORE_FLAGS) -std=gnu99 -w -Werror=implicit-function-declaration
EE_LDFLAGS := -L$(GSKIT)/lib
EE_LIBS = -lgskit -ldmakit -ldebug -lpad -laudsrv -lfileXio -lpatches -lm

all: $(EE_BIN)

clean:
	rm -rf build $(EE_BIN)

# p02/p07: as partes pesadas do nucleo com -O3: video (fundo, sprites,
# mistura), CPU, CD e som. O resto (libretro.c, arquivos, CD de imagem) -O2.
O3_OBJS = build/$(P)/vdc.o build/$(P)/huc6280.o build/$(P)/pcecd.o build/$(P)/pcecd_drive.o build/$(P)/psg.o
$(O3_OBJS): build/$(P)/%.o: $(P)/%.c
	@mkdir -p $(dir $@)
	$(EE_CC) $(EE_CFLAGS) -O3 $(EE_INCS) -c $< -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

build/cdusb/%.o: $(CDUSB)/ee/%.c $(CDUSB)/ee/cdcache.h $(CDUSB)/ee/cdusb_ee.h
	@mkdir -p $(dir $@)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

build/src/main.o build/src/ps2_cd.o build/src/ui.o: src/ui.h $(CDUSB)/ee/cdcache.h $(CDUSB)/ee/cdusb_ee.h $(CDUSB)/common/cdusb_rpc.h

build/gen/%.o: build/gen/%.c
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

build/gen/syscard.c: res/syscard3.pce tools/mkbin.py
	@mkdir -p build/gen
	python3 tools/mkbin.py $< $@ ps2_syscard3

build/gen/irx_iomanx.c: $(IRXDIR)/iomanX.irx
	@mkdir -p build/gen
	$(BIN2C) $< $@ irx_iomanx
build/gen/irx_filexio.c: $(IRXDIR)/fileXio.irx
	@mkdir -p build/gen
	$(BIN2C) $< $@ irx_filexio
build/gen/irx_usbd.c: $(IRXDIR)/usbd.irx
	@mkdir -p build/gen
	$(BIN2C) $< $@ irx_usbd
build/gen/irx_bdm.c: $(IRXDIR)/bdm.irx
	@mkdir -p build/gen
	$(BIN2C) $< $@ irx_bdm
build/gen/irx_bdmfs_fatfs.c: $(IRXDIR)/bdmfs_fatfs.irx
	@mkdir -p build/gen
	$(BIN2C) $< $@ irx_bdmfs_fatfs
build/gen/irx_usbmass_bd.c: $(IRXDIR)/usbmass_bd.irx
	@mkdir -p build/gen
	$(BIN2C) $< $@ irx_usbmass_bd
build/gen/irx_audsrv.c: $(IRXDIR)/audsrv.irx
	@mkdir -p build/gen
	$(BIN2C) $< $@ irx_audsrv
build/gen/irx_cdusb.c: $(CDUSB)/iop/cdusb.irx
	@mkdir -p build/gen
	$(BIN2C) $< $@ irx_cdusb

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

# PS2USB.ELF - Retrovolt. Leitor de CDVD Externo (EE)
# Compilar: /opt/ps2/ps2.sh <repositorio> "make -C cdusb/iop && make -C neutrinousb/iop && make -C emberusb/iop && make -C emberusb/loader && make -C neutrinousb/ee final"
# 'final' = strip + selo de integridade (tools/selo.py) + compactacao LZMA (ps2-packer) -> PS2USB_FINAL.ELF
# make LOG=1 ... = versao de diagnostico com log em mass:/PS2USB.TXT
EE_BIN = PS2USB.ELF
FINAL = PS2USB_FINAL.ELF
CDUSB = ../../cdusb
IRX_OBJS = irx_iomanx.o irx_filexio.o irx_usbd.o irx_cdusb.o irx_bdm.o irx_bdmfs_fatfs.o irx_usbmass_bd.o \
	   irx_usbcd_bd.o irx_vcd.o loader_elf.o
EE_OBJS = main.o cdusb_ee.o $(IRX_OBJS)
EE_INCS := -I$(CDUSB)/common -I$(CDUSB)/ee
EE_LIBS = -ldebug -lpad -lfileXio -lpatches

# o menor possivel
EE_OPTFLAGS = -Os
EE_DBGINFOFLAGS =
EE_CFLAGS += -ffunction-sections -fdata-sections
EE_LDFLAGS += -Wl,--gc-sections
NEWLIB_NANO = 1
EE_LINKFILE = linkfile
ifeq ($(LOG),1)
EE_CFLAGS += -DCOM_LOG
endif

BIN2C = $(PS2SDK)/bin/bin2c
IRXDIR = $(PS2SDK)/iop/irx
# os blobs viram const: ficam no .rodata, dentro do selo
CONST = sed -i 's/^unsigned char /const unsigned char /'

all: $(EE_BIN)

final: $(EE_BIN)
	$(EE_STRIP) -o selado.elf $(EE_BIN)
	python3 ../tools/selo.py selado.elf
	ps2-packer selado.elf $(FINAL)
	rm -f selado.elf
	ls -l $(FINAL)

clean:
	rm -f $(EE_BIN) $(FINAL) $(EE_OBJS) irx_*.c loader_elf.c loader_strip.elf selado.elf

cdusb_ee.o: $(CDUSB)/ee/cdusb_ee.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

irx_iomanx.c: $(IRXDIR)/iomanX.irx
	$(BIN2C) $< $@ irx_iomanx && $(CONST) $@
irx_filexio.c: $(IRXDIR)/fileXio.irx
	$(BIN2C) $< $@ irx_filexio && $(CONST) $@
irx_usbd.c: $(IRXDIR)/usbd.irx
	$(BIN2C) $< $@ irx_usbd && $(CONST) $@
irx_bdm.c: $(IRXDIR)/bdm.irx
	$(BIN2C) $< $@ irx_bdm && $(CONST) $@
irx_bdmfs_fatfs.c: $(IRXDIR)/bdmfs_fatfs.irx
	$(BIN2C) $< $@ irx_bdmfs_fatfs && $(CONST) $@
irx_usbmass_bd.c: $(IRXDIR)/usbmass_bd.irx
	$(BIN2C) $< $@ irx_usbmass_bd && $(CONST) $@
irx_cdusb.c: $(CDUSB)/iop/cdusb.irx
	$(BIN2C) $< $@ irx_cdusb && $(CONST) $@
irx_usbcd_bd.c: ../iop/usbcd_bd.irx
	$(BIN2C) $< $@ irx_usbcd_bd && $(CONST) $@

# PS1: pasta virtual do Ember (o CD da leitora vira .cue/.bin)
irx_vcd.c: ../../emberusb/iop/vcd.irx
	$(BIN2C) $< $@ irx_vcd && $(CONST) $@
loader_strip.elf: ../../emberusb/loader/loader.elf
	$(EE_STRIP) -o $@ $<
loader_elf.c: loader_strip.elf
	$(BIN2C) $< $@ loader_elf && $(CONST) $@

main.o cdusb_ee.o: $(CDUSB)/common/cdusb_rpc.h $(CDUSB)/ee/cdusb_ee.h

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

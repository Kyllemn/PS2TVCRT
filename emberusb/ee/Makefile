# EMBERUSB.ELF - abre o Ember com o CD de PS1 da leitora USB (EE)
# Compilar: /opt/ps2/ps2.sh <repositorio> "make -C cdusb/iop && make -C emberusb/iop && make -C emberusb/loader && make -C emberusb/ee"
EE_BIN = EMBERUSB.ELF
CDUSB = ../../cdusb
IRX_OBJS = irx_iomanx.o irx_filexio.o irx_usbd.o irx_cdusb.o irx_bdm.o irx_bdmfs_fatfs.o irx_usbmass_bd.o irx_vcd.o loader_elf.o
EE_OBJS = main.o cdusb_ee.o $(IRX_OBJS)
EE_INCS := -I$(CDUSB)/common -I$(CDUSB)/ee
EE_LIBS = -ldebug -lpad -lfileXio -lpatches

BIN2C = $(PS2SDK)/bin/bin2c
IRXDIR = $(PS2SDK)/iop/irx

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS) irx_*.c loader_elf.c

cdusb_ee.o: $(CDUSB)/ee/cdusb_ee.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

irx_iomanx.c: $(IRXDIR)/iomanX.irx
	$(BIN2C) $< $@ irx_iomanx
irx_filexio.c: $(IRXDIR)/fileXio.irx
	$(BIN2C) $< $@ irx_filexio
irx_usbd.c: $(IRXDIR)/usbd.irx
	$(BIN2C) $< $@ irx_usbd
irx_bdm.c: $(IRXDIR)/bdm.irx
	$(BIN2C) $< $@ irx_bdm
irx_bdmfs_fatfs.c: $(IRXDIR)/bdmfs_fatfs.irx
	$(BIN2C) $< $@ irx_bdmfs_fatfs
irx_usbmass_bd.c: $(IRXDIR)/usbmass_bd.irx
	$(BIN2C) $< $@ irx_usbmass_bd
irx_cdusb.c: $(CDUSB)/iop/cdusb.irx
	$(BIN2C) $< $@ irx_cdusb
irx_vcd.c: ../iop/vcd.irx
	$(BIN2C) $< $@ irx_vcd
loader_elf.c: ../loader/loader.elf
	$(BIN2C) $< $@ loader_elf

main.o cdusb_ee.o: $(CDUSB)/common/cdusb_rpc.h $(CDUSB)/ee/cdusb_ee.h

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

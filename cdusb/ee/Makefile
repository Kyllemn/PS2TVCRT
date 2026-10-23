# CDUSB.ELF - diagnostico do leitor de CD USB (EE)
EE_BIN = CDUSB.ELF
IRX_OBJS = irx_iomanx.o irx_filexio.o irx_usbd.o irx_cdusb.o irx_bdm.o irx_bdmfs_fatfs.o irx_usbmass_bd.o
EE_OBJS = main.o diag.o cdusb_ee.o $(IRX_OBJS)
EE_INCS := -I../common
EE_LIBS = -ldebug -lpad -lfileXio -lpatches

BIN2C = $(PS2SDK)/bin/bin2c
IRXDIR = $(PS2SDK)/iop/irx

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS) irx_*.c

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
irx_cdusb.c: ../iop/cdusb.irx
	$(BIN2C) $< $@ irx_cdusb

main.o diag.o cdusb_ee.o: ../common/cdusb_rpc.h cdusb_ee.h diag.h

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

# usbcd_bd.irx - leitora de CD/DVD USB como dispositivo de bloco (BDM) para o Neutrino
IOP_BIN = usbcd_bd.irx
IOP_OBJS = usbcd_bd.o imports.o

all: $(IOP_BIN)

clean:
	rm -rf obj $(IOP_BIN)

include $(PS2SDK)/Defs.make
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal

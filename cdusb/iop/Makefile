# cdusb.irx - leitor de CD USB (IOP)
IOP_BIN = cdusb.irx
IOP_OBJS = exports.o cdusb.o imports.o
IOP_CFLAGS += -I../common

all: $(IOP_BIN)

clean:
	rm -rf obj $(IOP_BIN)

include $(PS2SDK)/Defs.make
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal

# vcd.irx - CD de PS1 da leitora USB como .cue/.bin para o Ember (IOP)
IOP_BIN = vcd.irx
IOP_OBJS = vcd.o imports.o
IOP_CFLAGS += -I../../cdusb/common

all: $(IOP_BIN)

clean:
	rm -rf obj $(IOP_BIN)

include $(PS2SDK)/Defs.make
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal

# loader.elf - mini-loader sem reiniciar o IOP (vai embutido no EMBERUSB.ELF)
EE_BIN = loader.elf
EE_OBJS = loader.o
EE_LINKFILE = linkfile
NEWLIB_NANO = 1
EE_CFLAGS += -Os

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

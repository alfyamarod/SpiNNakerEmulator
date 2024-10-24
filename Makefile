#     Makefile for spin_emu on Debian 7 i686
#     Copyright (C) 2013  Matthew Orlinski


#CFLAGS = -Wall -I../scamp -I../spin1_api  -I./ 
CFLAGS = -Wall -m32 -I./		 
#CFLAGS = -Wall -I../spinnaker/spinnaker_tools/scamp -I../spinnaker/spinnaker_tools/spin1_api -I../spinnaker/spinnaker_tools/include  -I./ 
LDFLAGS = -Wl,-Ttext-segment=0x30000000 -m32 -pthread
# -Wl,-Ttext-segment=0x30000000 // puts the program code at a different address from 0x400000, should not collide with any SpiNNaker usage

#OBJS = spin_emu.o spin_emu_dispatcher.o spin_emu_util.o spin_emu_api.o spin_emu_io.o scamp_emu.o  spin_emu_main.o spin_emu_app.o lcfg_static.o
OBJS = spin_emu.o spin_emu_dispatcher.o spin_emu_util.o spin_emu_api.o spin_emu_io.o scamp_emu.o  spin_emu_main.o lcfg_static.o

all: spin_emu
.PHONY: all

spin_emu: ${OBJS}
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)
$(OBJS): spin_emu.h

clean:
	rm -rf spin_emu spin_emu_app.o ${OBJS}

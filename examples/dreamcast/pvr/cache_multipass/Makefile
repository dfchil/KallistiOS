# KallistiOS ##version##
#
# examples/dreamcast/pvr/cache_multipass/Makefile
#

TARGETS = cache_multipass.elf cache_multipass_fsaa.elf
OBJS = $(TARGETS:.elf=.o)

all: rm-elf $(TARGETS)

include $(KOS_BASE)/Makefile.rules

cache_multipass_fsaa.o: cache_multipass.c
	kos-cc -DUSE_FSAA=1 -c $< -o $@

cache_multipass.o: cache_multipass.c
	kos-cc -DUSE_FSAA=0 -c $< -o $@

%.elf: %.o
	kos-cc -o $@ $<

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGETS)

run: cache_multipass.elf
	$(KOS_LOADER) cache_multipass.elf

dist: $(TARGETS)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGETS)

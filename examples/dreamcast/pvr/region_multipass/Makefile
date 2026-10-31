# KallistiOS ##version##
#
# examples/dreamcast/pvr/region_multipass/Makefile
#

TARGETS = region_multipass.elf region_multipass_fsaa.elf
OBJS = $(TARGETS:.elf=.o)

all: rm-elf $(TARGETS)

include $(KOS_BASE)/Makefile.rules

region_multipass_fsaa.o: region_multipass.c
	kos-cc -DUSE_FSAA=1 -c $< -o $@

region_multipass.o: region_multipass.c
	kos-cc -DUSE_FSAA=0 -c $< -o $@

%.elf: %.o
	kos-cc -o $@ $<

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGETS)

run: region_multipass.elf
	$(KOS_LOADER) region_multipass.elf

dist: $(TARGETS)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGETS)

# KallistiOS ##version##
#
# examples/dreamcast/pvr/region_listcont/Makefile
#

TARGETS = region_listcont.elf region_listcont_fsaa.elf
OBJS = $(TARGETS:.elf=.o)

all: rm-elf $(TARGETS)

include $(KOS_BASE)/Makefile.rules

region_listcont_fsaa.o: region_listcont.c
	kos-cc -DUSE_FSAA=1 -c $< -o $@

region_listcont.o: region_listcont.c
	kos-cc -DUSE_FSAA=0 -c $< -o $@

%.elf: %.o
	kos-cc -o $@ $<

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGETS)

run: region_listcont.elf
	$(KOS_LOADER) region_listcont.elf

dist: $(TARGETS)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGETS)

# KallistiOS ##version##
#
# examples/dreamcast/pvr/cache_listcont/Makefile
#

TARGETS = cache_listcont.elf cache_listcont_fsaa.elf
OBJS = $(TARGETS:.elf=.o)

all: rm-elf $(TARGETS)

include $(KOS_BASE)/Makefile.rules

cache_listcont_fsaa.o: cache_listcont.c
	kos-cc -DUSE_FSAA=1 -c $< -o $@

cache_listcont.o: cache_listcont.c
	kos-cc -DUSE_FSAA=0 -c $< -o $@

%.elf: %.o
	kos-cc -o $@ $<

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGETS)

run: cache_listcont.elf
	$(KOS_LOADER) cache_listcont.elf

dist: $(TARGETS)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGETS)

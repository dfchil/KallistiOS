# Hardware test for per-tile region array control. Builds one ELF per
# question and one with every question, each without and with FSAA.

NAMES = all q1 q2 q3 q4
TESTS_all = 0xf
TESTS_q1 = 0x1
TESTS_q2 = 0x2
TESTS_q3 = 0x4
TESTS_q4 = 0x8

TARGETS = $(NAMES:%=region_hwtest_%.elf) $(NAMES:%=region_hwtest_%_fsaa.elf)
OBJS = $(TARGETS:.elf=.o)

all: rm-elf $(TARGETS)

include $(KOS_BASE)/Makefile.rules

region_hwtest_%_fsaa.o: region_hwtest.c
	kos-cc -DTESTS=$(TESTS_$*) -DUSE_FSAA=1 -c $< -o $@

region_hwtest_%.o: region_hwtest.c
	kos-cc -DTESTS=$(TESTS_$*) -DUSE_FSAA=0 -c $< -o $@

%.elf: %.o
	kos-cc -o $@ $<

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGETS)

run: region_hwtest_all.elf
	$(KOS_LOADER) region_hwtest_all.elf

dist: $(TARGETS)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGETS)

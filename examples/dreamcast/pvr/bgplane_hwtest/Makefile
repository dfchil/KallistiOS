#
# bgplane_hwtest
# Hardware test for the background plane: read-back checks and visual pages.
# Copyright (C) 2026 Daniel Fairchild
#

TARGET = bgplane_hwtest.elf
OBJS = bgplane_hwtest.o

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS)

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

dist: $(TARGET)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGET)


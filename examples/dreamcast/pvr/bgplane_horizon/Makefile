#
# bgplane_horizon
# Sky gradient on the background plane that follows the horizon of a flying camera.
# Copyright (C) 2026 Daniel Fairchild
#

TARGET = bgplane_horizon.elf
OBJS = bgplane_horizon.o

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


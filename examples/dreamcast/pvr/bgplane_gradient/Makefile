#
# bgplane_gradient
# Vertical sky gradient on the background plane, behind a rotating cube.
# Copyright (C) 2026 Daniel Fairchild
#

TARGET = bgplane_gradient.elf
OBJS = bgplane_gradient.o

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


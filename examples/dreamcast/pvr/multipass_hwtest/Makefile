# KallistiOS ##version##
#
# examples/dreamcast/pvr/multipass_hwtest/Makefile
#
# Builds multipass_hwtest.elf (no FSAA) and multipass_hwtest_fsaa.elf (FSAA).

TARGET = multipass_hwtest.elf
TARGET_FSAA = multipass_hwtest_fsaa.elf
OBJS = multipass_hwtest.o
OBJS_FSAA = multipass_hwtest_fsaa.o

all: rm-elf $(TARGET) $(TARGET_FSAA)

include $(KOS_BASE)/Makefile.rules

clean: rm-elf
	-rm -f $(OBJS) $(OBJS_FSAA)

rm-elf:
	-rm -f $(TARGET) $(TARGET_FSAA)

multipass_hwtest_fsaa.o: multipass_hwtest.c
	kos-cc $(CFLAGS) -DHWTEST_FSAA=1 -c $< -o $@

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS)

$(TARGET_FSAA): $(OBJS_FSAA)
	kos-cc -o $(TARGET_FSAA) $(OBJS_FSAA)

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

dist: $(TARGET) $(TARGET_FSAA)
	-rm -f $(OBJS) $(OBJS_FSAA)
	$(KOS_STRIP) $(TARGET) $(TARGET_FSAA)

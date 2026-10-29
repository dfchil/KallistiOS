TARGET = cache_hwtest.elf
OBJS = cache_hwtest.o

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

ifdef HWTEST_ONLY
KOS_CFLAGS += -DHWTEST_ONLY=$(HWTEST_ONLY)
endif

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

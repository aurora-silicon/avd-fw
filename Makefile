CC = clang
OBJCOPY = llvm-objcopy

AVD_VER := 3
AVD_TIER := 1

PAD = 0x12000

# avd-version = 3 in device tree
ifeq ($(AVD_VER),2)
PAD = 0xc000
endif

ifeq ($(AVD_VER),3)
PAD = 0x10000
endif

ifeq ($(AVD_VER),5)
ifeq ($(AVD_TIER),0)
PAD = 0x10000
endif
endif

LD_SCRIPT = avd-cm3.ld
CFLAGS = -Wall -O2 -nostdlib -MMD -MP \
	 -mthumb -mcpu=cortex-m3 --target=arm-none-eabi \
	 -DAVD_VER=$(AVD_VER) -DAVD_TIER=$(AVD_TIER)

NAME = avd-fw-v$(AVD_VER)-t$(AVD_TIER)

# Keep the reset path at the start of .text.  The AVD CM3 boot path used on
# t8140 is only hardware-verified with _start at 0x0c70; placing avd.o last
# moves it behind the full IRQ trampoline table.
OBJECTS := avd.o irq.o util.o queue.o queue-fw.o

BUILD_OBJS := $(patsubst %,build/$(NAME)/%,$(OBJECTS))
FLAGS_FILE := build/$(NAME)/.flags

.PHONY: all clean FORCE
all: build/$(NAME).bin
clean:
	rm -rf build/*

build/$(NAME).elf: $(BUILD_OBJS) $(LD_SCRIPT)
	$(CC) $(CFLAGS) -fuse-ld=lld -Wl,--defsym=AVD_IMAGE_SIZE=$(PAD) -T $(LD_SCRIPT) -o $@ $(BUILD_OBJS)

# Check flags without changing their timestamp when the command is unchanged.
# Per-variant objects avoid timestamp races between successive matrix builds.
FORCE:
$(FLAGS_FILE): FORCE
	mkdir -p "$(dir $@)"
	@printf '%s\n' "$(CC) $(CFLAGS) AVD_IMAGE_SIZE=$(PAD)" | cmp -s $@ - || printf '%s\n' "$(CC) $(CFLAGS) AVD_IMAGE_SIZE=$(PAD)" > $@

build/$(NAME)/%.o: src/%.c $(FLAGS_FILE)
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c -o $@ $<

build/$(NAME).bin: build/$(NAME).elf
	$(OBJCOPY) -O binary --strip-debug --pad-to=$(PAD) $< $@

HOSTCC ?= cc
.PHONY: test
test:
	mkdir -p build/tests
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -O2 -Isrc src/queue.c tests/queue-test.c -o build/tests/queue-test
	build/tests/queue-test

-include $(BUILD_OBJS:.o=.d)

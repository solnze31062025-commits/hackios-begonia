# Stage-2 loader for begonia (bare-metal aarch64).
# Builds on GitHub runners (clang + lld available) and anywhere LLVM is.

TARGET  := aarch64-none-elf
CC      := clang
LDFLAGS_TOOL := ld.lld
OBJCOPY := llvm-objcopy

CFLAGS := --target=$(TARGET) -march=armv8-a -mgeneral-regs-only \
          -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-asynchronous-unwind-tables -O2 -g \
          -Wall -Wextra -Wno-unused-parameter -Wno-unused-function

OBJS := image.o crt0.o uart.o dt.o mmu.o fb.o font4x6.o plog.o fdt.o main.o

all: loader.bin

%.o: %.S loader.h
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

%.o: %.c loader.h
	$(CC) $(CFLAGS) -c $< -o $@

loader.elf: $(OBJS) loader.ld
	$(LDFLAGS_TOOL) -T loader.ld -nostdlib -z max-page-size=4096 \
	    -o $@ $(OBJS)

loader.bin: loader.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f $(OBJS) loader.elf loader.bin

.PHONY: all clean

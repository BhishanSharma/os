# ---------------------------------------------------------------------------
# Terminal OS build system
#
# Run `make help` to see every target.
# Normally this runs INSIDE the Docker build container (see docs/SETUP.md),
# which provides x86_64-elf-gcc, nasm, and grub-mkrescue.
# ---------------------------------------------------------------------------

THIS_MAKEFILE := $(firstword $(MAKEFILE_LIST))

# ---- Toolchain (override on the command line, e.g. `make CROSS=`) ---------
CROSS ?= x86_64-elf-
CC    := $(CROSS)gcc
LD    := $(CROSS)ld
NASM  ?= nasm
QEMU  ?= qemu-system-x86_64

# ---- Layout ---------------------------------------------------------------
SRC_KERNEL := src/impl/kernel
SRC_X86    := src/impl/x86_64
INCLUDES   := src/intf
LINKER_LD  := targets/x86_64/linker.ld
ISO_SRC    := targets/x86_64/iso
BUILD      := build
DIST       := dist/x86_64
ISO_STAGE  := $(BUILD)/iso
KERNEL_BIN := $(DIST)/kernel.bin
KERNEL_ISO := $(DIST)/kernel.iso
DISK_IMG   ?= disk.img

# ---- Flags ----------------------------------------------------------------
# -mno-red-zone: required for kernel code that takes interrupts (the CPU
#                pushes onto the stack below rsp, which the red zone assumes
#                is private to the function).
CFLAGS := -c -I $(INCLUDES) -ffreestanding -fno-stack-protector -fno-pie \
          -mno-red-zone -Wall -Wextra -Wno-unused-parameter
ASFLAGS := -f elf64
LDFLAGS := -n -T $(LINKER_LD)

# `make DEBUG=1` -> debug symbols, no optimisation (needed for gdb).
ifeq ($(DEBUG),1)
CFLAGS += -g -O0
ASFLAGS += -g -F dwarf
endif

# Treat warnings as errors with `make WERROR=1` (used by CI once clean).
ifeq ($(WERROR),1)
CFLAGS += -Werror
endif

# ---- Source discovery -----------------------------------------------------
kernel_c   := $(shell find $(SRC_KERNEL) -name '*.c')
x86_c      := $(shell find $(SRC_X86) -name '*.c')
x86_asm    := $(shell find $(SRC_X86) -name '*.asm')

kernel_obj := $(patsubst $(SRC_KERNEL)/%.c,$(BUILD)/kernel/%.o,$(kernel_c))
x86_c_obj  := $(patsubst $(SRC_X86)/%.c,$(BUILD)/x86_64/%.o,$(x86_c))
x86_asm_obj:= $(patsubst $(SRC_X86)/%.asm,$(BUILD)/x86_64/%.o,$(x86_asm))
objects    := $(kernel_obj) $(x86_c_obj) $(x86_asm_obj)

# The kernel image starts at 1 MiB and must end before the heap at 2 MiB.
# linker.ld exports kernel_end and ASSERTs this limit, so an oversized kernel
# fails to link; `make size` just shows how much headroom is left.
KERNEL_BASE  := 1048576
KERNEL_LIMIT := 2097152

# ---- Targets --------------------------------------------------------------
.PHONY: all build-x86_64 iso run run-nodisk debug disk size clean help
.DEFAULT_GOAL := help

help: ## Show this help
	@echo "Terminal OS - make targets:"
	@grep -hE '^[a-zA-Z0-9_-]+:.*## ' $(THIS_MAKEFILE) | \
		awk -F':.*## ' '{printf "  %-14s %s\n", $$1, $$2}'
	@echo ""
	@echo "Options: DEBUG=1  WERROR=1  CROSS=<prefix>  DISK_IMG=<file>"

all: build-x86_64 ## Alias for build-x86_64

build-x86_64: $(KERNEL_ISO) size ## Build dist/x86_64/kernel.iso (run inside Docker)

iso: build-x86_64 ## Alias for build-x86_64

# C sources
$(BUILD)/kernel/%.o: $(SRC_KERNEL)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP $< -o $@

$(BUILD)/x86_64/%.o: $(SRC_X86)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP $< -o $@

# Assembly sources. (Make tries the .c rule first, finds no .c file, then falls
# back to this rule, so keep a .c and .asm file from sharing a name in one folder.)
$(BUILD)/x86_64/%.o: $(SRC_X86)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@

# Link
$(KERNEL_BIN): $(objects) $(LINKER_LD)
	@mkdir -p $(DIST)
	$(LD) $(LDFLAGS) -o $@ $(objects)

# ISO: staged in build/iso so the source tree stays clean.
# (/usr/lib/grub/i386-pc is passed through to xorriso, as in the original build.)
$(KERNEL_ISO): $(KERNEL_BIN) $(ISO_SRC)/boot/grub/grub.cfg
	rm -rf $(ISO_STAGE)
	mkdir -p $(ISO_STAGE)
	cp -r $(ISO_SRC)/. $(ISO_STAGE)/
	mkdir -p $(ISO_STAGE)/boot
	cp $(KERNEL_BIN) $(ISO_STAGE)/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o $@ $(ISO_STAGE)

size: $(KERNEL_BIN) ## Print kernel size and remaining headroom below the heap
	@$(CROSS)size $(KERNEL_BIN)
	@end=$$($(CROSS)nm $(KERNEL_BIN) | awk '$$3=="kernel_end"{print "0x"$$1}'); \
	used=$$(( end - $(KERNEL_BASE) )); free=$$(( $(KERNEL_LIMIT) - end )); \
	echo "kernel image: $$used bytes mapped, $$free bytes of headroom (limit $$(( $(KERNEL_LIMIT) - $(KERNEL_BASE) )))"

# ---- Host-side helpers (run these on your machine, NOT in Docker) ----------
disk: ## Create a 32 MB FAT32 test disk (needs dosfstools + mtools)
	sh scripts/mkdisk.sh $(DISK_IMG)

run: ## Boot the ISO in QEMU with disk + NIC (host-side; build first)
	@test -f $(KERNEL_ISO) || { echo "No $(KERNEL_ISO): run 'make build-x86_64' (in Docker) first"; exit 1; }
	$(QEMU) -cdrom $(KERNEL_ISO) \
	    -drive file=$(DISK_IMG),format=raw,index=0,media=disk -boot d \
	    -device rtl8139,netdev=n0 -netdev user,id=n0

run-nodisk: ## Boot the ISO in QEMU without a disk (host-side)
	@test -f $(KERNEL_ISO) || { echo "No $(KERNEL_ISO): run 'make build-x86_64' (in Docker) first"; exit 1; }
	$(QEMU) -cdrom $(KERNEL_ISO) -boot d

debug: ## Boot paused with a gdb server on :1234 (build with DEBUG=1 first)
	@test -f $(KERNEL_ISO) || { echo "No $(KERNEL_ISO): run 'make DEBUG=1 build-x86_64' (in Docker) first"; exit 1; }
	$(QEMU) -cdrom $(KERNEL_ISO) \
	    -drive file=$(DISK_IMG),format=raw,index=0,media=disk -boot d \
	    -device rtl8139,netdev=n0 -netdev user,id=n0 -s -S

clean: ## Remove build/ and dist/
	rm -rf $(BUILD) dist

# Header dependency tracking: editing a .h now rebuilds what includes it.
-include $(objects:.o=.d)

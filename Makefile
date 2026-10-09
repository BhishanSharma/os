# ---------------------------------------------------------------------------
# Terminal OS build system
#
# Run `make help` to see every target.
# Normally this runs INSIDE the Docker build container (see docs/SETUP.md),
# which provides x86_64-elf-gcc, nasm, and grub-mkrescue.
# ---------------------------------------------------------------------------

THIS_MAKEFILE := $(firstword $(MAKEFILE_LIST))

# ---- Toolchain ------------------------------------------------------------
CROSS ?= x86_64-elf-
CC    := $(CROSS)gcc
CXX   := $(CROSS)g++
LD    := $(CROSS)ld
NASM  ?= nasm
QEMU  ?= qemu-system-x86_64

# ---- Layout ---------------------------------------------------------------
SRC_KERNEL := src/impl/kernel
SRC_X86    := src/impl/x86_64

# Include directories used by both C and C++ code.
INCLUDES   := src/intf src/intf/lib

BEARSSL_DIR ?= /root/bearssl
BEARSSL_BUILD = $(BUILD)/bearssl
BEARSSL_LIB = $(BEARSSL_BUILD)/libbearssl.a

LINKER_LD  := targets/x86_64/linker.ld
ISO_SRC    := targets/x86_64/iso

BUILD      := build
DIST       := dist/x86_64
ISO_STAGE  := $(BUILD)/iso

KERNEL_BIN := $(DIST)/kernel.bin
KERNEL_ISO := $(DIST)/kernel.iso

DISK_IMG   ?= disk.img

# ---- Flags ----------------------------------------------------------------
#
# -ffreestanding:
#   This is a kernel, so we don't assume a hosted C/C++ environment.
#
# -mno-red-zone:
#   Required for interrupt-safe x86_64 kernel code.
#
# -ffunction-sections / -fdata-sections:
#   Allows the linker to discard unused code with --gc-sections.
#

INCLUDE_FLAGS := $(addprefix -I ,$(INCLUDES)) -I $(BEARSSL_DIR)/inc

CFLAGS := -c $(INCLUDE_FLAGS) \
          -ffreestanding \
          -fno-stack-protector \
          -fno-pie \
          -mno-red-zone \
          -Wall \
          -Wextra \
          -Wno-unused-parameter \
          -ffunction-sections \
          -fdata-sections

CXXFLAGS := -c -I buildenv/freestanding/include $(INCLUDE_FLAGS) \
            -ffreestanding \
            -fno-stack-protector \
            -fno-pie \
            -fno-exceptions \
            -fno-rtti \
            -fno-threadsafe-statics \
            -fno-unwind-tables \
            -fno-asynchronous-unwind-tables \
            -mno-red-zone \
            -Os \
            -Wall \
            -Wextra \
            -Wno-unused-parameter \
            -ffunction-sections \
            -fdata-sections

ASFLAGS := -f elf64

LDFLAGS := -n -T $(LINKER_LD) --gc-sections

# `make DEBUG=1` -> debug symbols, no optimisation.
ifeq ($(DEBUG),1)
CFLAGS += -g -O0
CXXFLAGS += -g -O0
ASFLAGS += -g -F dwarf
endif

# Treat warnings as errors with `make WERROR=1`.
ifeq ($(WERROR),1)
CFLAGS += -Werror
CXXFLAGS += -Werror
endif

# ---- Source discovery -----------------------------------------------------

kernel_c := $(shell find $(SRC_KERNEL) -name '*.c')
x86_c    := $(shell find $(SRC_X86) -name '*.c')
x86_asm  := $(shell find $(SRC_X86) -name '*.asm')
x86_cpp  := $(shell find $(SRC_X86) -name '*.cpp')

kernel_obj := $(patsubst $(SRC_KERNEL)/%.c,$(BUILD)/kernel/%.o,$(kernel_c))
x86_c_obj  := $(patsubst $(SRC_X86)/%.c,$(BUILD)/x86_64/%.o,$(x86_c))
x86_asm_obj := $(patsubst $(SRC_X86)/%.asm,$(BUILD)/x86_64/%.o,$(x86_asm))
x86_cpp_obj := $(patsubst $(SRC_X86)/%.cpp,$(BUILD)/x86_64/%.o,$(x86_cpp))

objects := $(kernel_obj) $(x86_c_obj) $(x86_cpp_obj) $(x86_asm_obj)

# The kernel image starts at 1 MiB and must end before the heap at 2 MiB.
KERNEL_BASE  := 1048576
KERNEL_LIMIT := 2097152

# ---- Targets --------------------------------------------------------------

.PHONY: all build-x86_64 iso run run-nodisk debug disk size clean help test-net

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


# ---- C sources ------------------------------------------------------------

$(BUILD)/kernel/%.o: $(SRC_KERNEL)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP $< -o $@


$(BUILD)/x86_64/%.o: $(SRC_X86)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP $< -o $@


# ---- C++ sources ----------------------------------------------------------
#
# Used by the HTTPS/TLS BearSSL adapter.
#

$(BUILD)/x86_64/%.o: $(SRC_X86)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP $< -o $@


# ---- Assembly sources ------------------------------------------------------

$(BUILD)/x86_64/%.o: $(SRC_X86)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASFLAGS) $< -o $@


# ---- Link -----------------------------------------------------------------

$(BEARSSL_LIB):
	$(MAKE) -C $(BEARSSL_DIR) \
	    BUILD=$(abspath $(BEARSSL_BUILD)) \
	    CC=$(CC) AR=$(CROSS)ar \
	    CFLAGS="-ffreestanding -fno-builtin -fno-stack-protector -fno-pie -mno-red-zone -Os -ffunction-sections -fdata-sections" \
	    INCFLAGS="-Isrc -Iinc -I$(abspath buildenv/freestanding/include)" \
	    STATICLIB=lib DLL=no TOOLS=no TESTS=no


$(KERNEL_BIN): $(objects) $(BEARSSL_LIB) $(LINKER_LD)
	@mkdir -p $(DIST)
	$(LD) $(LDFLAGS) -o $@ $(objects) $(BEARSSL_LIB)


# ---- ISO ------------------------------------------------------------------

$(KERNEL_ISO): $(KERNEL_BIN) $(ISO_SRC)/boot/grub/grub.cfg
	rm -rf $(ISO_STAGE)
	mkdir -p $(ISO_STAGE)
	cp -r $(ISO_SRC)/. $(ISO_STAGE)/
	mkdir -p $(ISO_STAGE)/boot
	cp $(KERNEL_BIN) $(ISO_STAGE)/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o $@ $(ISO_STAGE)


# ---- Size -----------------------------------------------------------------

size: $(KERNEL_BIN) ## Print kernel size and remaining headroom below the heap
	@$(CROSS)size $(KERNEL_BIN)
	@end=$$($(CROSS)nm $(KERNEL_BIN) | \
		awk '$$3=="kernel_end"{print "0x"$$1}'); \
	used=$$(( end - $(KERNEL_BASE) )); \
	free=$$(( $(KERNEL_LIMIT) - end )); \
	echo "kernel image: $$used bytes mapped, $$free bytes of headroom (limit $$(( $(KERNEL_LIMIT) - $(KERNEL_BASE) )))"


# ---- Host-side helpers ----------------------------------------------------

disk: ## Create a 32 MB FAT32 test disk (needs dosfstools + mtools)
	sh scripts/mkdisk.sh $(DISK_IMG)


run: ## Boot the ISO in QEMU with disk + NIC (host-side; build first)
	@test -f $(KERNEL_ISO) || { \
		echo "No $(KERNEL_ISO): run 'make build-x86_64' (in Docker) first"; \
		exit 1; \
	}
	$(QEMU) -cpu max -cdrom $(KERNEL_ISO) \
	    -drive file=$(DISK_IMG),format=raw,index=0,media=disk -boot d \
	    -device rtl8139,netdev=n0 -netdev user,id=n0 -serial stdio


run-nodisk: ## Boot the ISO in QEMU without a disk (host-side)
	@test -f $(KERNEL_ISO) || { \
		echo "No $(KERNEL_ISO): run 'make build-x86_64' (in Docker) first"; \
		exit 1; \
	}
	$(QEMU) -cpu max -cdrom $(KERNEL_ISO) -boot d -serial stdio


debug: ## Boot paused with a gdb server on :1234
	@test -f $(KERNEL_ISO) || { \
		echo "No $(KERNEL_ISO): run 'make DEBUG=1 build-x86_64' (in Docker) first"; \
		exit 1; \
	}
	$(QEMU) -cpu max -cdrom $(KERNEL_ISO) \
	    -drive file=$(DISK_IMG),format=raw,index=0,media=disk -boot d \
	    -device rtl8139,netdev=n0 -netdev user,id=n0 \
	    -serial stdio -s -S


# ---- Network host test ----------------------------------------------------

test-net: ## Unit-test the IP/ICMP/ARP code on the host with a simulated gateway
	mkdir -p $(BUILD)/test
	gcc -g -O1 -Wall -Wextra -Wno-unused-parameter \
	    -fsanitize=address,undefined \
	    -DNET_HOST_TEST \
	    -I src/intf \
	    tests/net_host_test.c \
	    $(SRC_X86)/net/net.c \
	    -o $(BUILD)/test/net_host_test
	$(BUILD)/test/net_host_test


# ---- Clean ----------------------------------------------------------------

clean: ## Remove build/ and dist/
	rm -rf $(BUILD) dist


# ---- Header dependency tracking -------------------------------------------

-include $(objects:.o=.d)

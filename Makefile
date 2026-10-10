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

# FAT32 image GRUB loads into RAM at boot (files for PCs without an IDE disk).
RAMDISK_DIR := targets/x86_64/ramdisk
RAMDISK_MB  ?= 40
RAMDISK_GZ  := $(BUILD)/ramdisk.img.gz
RAMDISK_STAGE := $(BUILD)/ramdisk-files

# ---- User programs (ring 3) -------------------------------------------------
# Static ELF executables linked at 1 GiB against the small C library in
# user/lib. Every user/programs/NAME.c becomes NAME.elf on the RAM disk; typing
# NAME in the shell runs it.
USER_BUILD  := $(BUILD)/user
USER_CFLAGS := -c -I user/include -I src/intf \
               -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
               -fno-pie -fno-stack-protector -mgeneral-regs-only \
               -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Wno-unused-parameter
user_lib_obj := $(patsubst user/lib/%.c,$(USER_BUILD)/lib/%.o,$(wildcard user/lib/*.c))
user_progs   := $(patsubst user/programs/%.c,%,$(wildcard user/programs/*.c))
USER_ELFS    := $(addprefix $(USER_BUILD)/,$(addsuffix .elf,$(user_progs)))

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

.PHONY: all build-x86_64 iso run run-nodisk debug disk size clean help test-net user

.DEFAULT_GOAL := help

help: ## Show this help
	@echo "Terminal OS - make targets:"
	@grep -hE '^[a-zA-Z0-9_-]+:.*## ' $(THIS_MAKEFILE) | \
		awk -F':.*## ' '{printf "  %-14s %s\n", $$1, $$2}'
	@echo ""
	@echo "Options: DEBUG=1  WERROR=1  CROSS=<prefix>  DISK_IMG=<file>  RAMDISK_MB=<n>"

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

$(RAMDISK_GZ): scripts/mkramdisk.sh $(wildcard $(RAMDISK_DIR)/*) $(USER_ELFS) $(THIS_MAKEFILE)
	rm -rf $(RAMDISK_STAGE)
	mkdir -p $(RAMDISK_STAGE)
	cp $(RAMDISK_DIR)/* $(USER_ELFS) $(RAMDISK_STAGE)/
	sh scripts/mkramdisk.sh $(RAMDISK_STAGE) $(RAMDISK_MB) $@


# ---- User programs ----------------------------------------------------------

user: $(USER_ELFS) ## Build the user programs (build/user/*.elf)

# Keep the program objects (make would delete them as intermediates).
.SECONDARY:

$(USER_BUILD)/lib/%.o: user/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP $< -o $@

$(USER_BUILD)/programs/%.o: user/programs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP $< -o $@

$(USER_BUILD)/crt0.o: user/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(USER_BUILD)/libc.a: $(user_lib_obj)
	$(CROSS)ar rcs $@ $^

$(USER_BUILD)/%.elf: $(USER_BUILD)/programs/%.o $(USER_BUILD)/crt0.o $(USER_BUILD)/libc.a user/user.ld
	$(LD) -T user/user.ld -z max-page-size=4096 -z noexecstack -o $@ $(USER_BUILD)/crt0.o $< $(USER_BUILD)/libc.a


$(KERNEL_ISO): $(KERNEL_BIN) $(RAMDISK_GZ) $(ISO_SRC)/boot/grub/grub.cfg
	rm -rf $(ISO_STAGE)
	mkdir -p $(ISO_STAGE)
	cp -r $(ISO_SRC)/. $(ISO_STAGE)/
	mkdir -p $(ISO_STAGE)/boot
	cp $(KERNEL_BIN) $(ISO_STAGE)/boot/kernel.bin
	cp $(RAMDISK_GZ) $(ISO_STAGE)/boot/ramdisk.img.gz
	grub-mkrescue -o $@ $(ISO_STAGE)   # BIOS + UEFI (all installed GRUB platforms)


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
	$(QEMU) -cpu max -m 512M -cdrom $(KERNEL_ISO) \
	    -drive file=$(DISK_IMG),format=raw,index=0,media=disk -boot d \
	    -device rtl8139,netdev=n0 -netdev user,id=n0 -serial stdio


run-nodisk: ## Boot the ISO in QEMU without a disk (host-side)
	@test -f $(KERNEL_ISO) || { \
		echo "No $(KERNEL_ISO): run 'make build-x86_64' (in Docker) first"; \
		exit 1; \
	}
	$(QEMU) -cpu max -m 512M -cdrom $(KERNEL_ISO) -boot d -serial stdio


debug: ## Boot paused with a gdb server on :1234
	@test -f $(KERNEL_ISO) || { \
		echo "No $(KERNEL_ISO): run 'make DEBUG=1 build-x86_64' (in Docker) first"; \
		exit 1; \
	}
	$(QEMU) -cpu max -m 512M -cdrom $(KERNEL_ISO) \
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
	    $(SRC_X86)/net/net.c $(SRC_X86)/net/tcp.c \
	    -o $(BUILD)/test/net_host_test
	$(BUILD)/test/net_host_test

test-tcp: ## Unit-test TCP on the host against a simulated peer (loss, reordering)
	mkdir -p $(BUILD)/test
	gcc -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined -I src/intf \
	    tests/tcp_host_test.c $(SRC_X86)/net/tcp.c -o $(BUILD)/test/tcp_host_test
	$(BUILD)/test/tcp_host_test

test-wpa: ## Unit-test the WPA2 handshake on the host (IEEE/RFC vectors, simulated AP)
	mkdir -p $(BUILD)/test
	$(MAKE) -C $(BEARSSL_DIR) -s lib >/dev/null      # a host build of BearSSL
	gcc -g -O1 -Wall -I src/intf -I $(BEARSSL_DIR)/inc tests/wpa_host_test.c $(SRC_X86)/net/wpa.c \
	    $(BEARSSL_DIR)/build/libbearssl.a -o $(BUILD)/test/wpa_host_test
	$(BUILD)/test/wpa_host_test


# ---- Clean ----------------------------------------------------------------

clean: ## Remove build/ and dist/
	rm -rf $(BUILD) dist


# ---- Header dependency tracking -------------------------------------------

-include $(objects:.o=.d) $(user_lib_obj:.o=.d) $(patsubst %.elf,%.d,$(subst $(USER_BUILD)/,$(USER_BUILD)/programs/,$(USER_ELFS)))

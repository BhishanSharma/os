# ---------------------------------------------------------------------------
# AddressSanitizer can fail at random under 32-bit mmap randomisation (Docker
# Desktop's kernel): run the host tests without ASLR when the system allows it
NOASLR := $(shell setarch -R true 2>/dev/null && echo setarch -R)

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
# UEFI firmware for QEMU (`make run`); on Linux e.g. /usr/share/OVMF/OVMF_CODE.fd
OVMF  ?= C:/Program Files/qemu/share/edk2-x86_64-code.fd

# ---- Layout ---------------------------------------------------------------
SRC_KERNEL := src/impl/kernel
SRC_X86    := src/impl/x86_64

# Include directories used by both C and C++ code.
INCLUDES   := src/intf src/intf/lib third_party/uacpi/include

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

# Terminal OS's own UEFI boot loader (loader/), and the USB stick image.
LOADER_EFI := $(BUILD)/loader/TERMOS.EFI
USB_IMG    := $(DIST)/terminalos.img
LOADER_CC  := x86_64-w64-mingw32-gcc

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

# Apps: every folder apps/NAME/ with an app.cfg. All its .c files link into
# NAME.elf, and app.cfg puts it in the Dock (DOCK.CFG). See docs/APPS.md.
app_names    := $(patsubst apps/%/app.cfg,%,$(wildcard apps/*/app.cfg))
APP_ELFS     := $(addprefix $(USER_BUILD)/,$(addsuffix .elf,$(app_names)))
app_objs      = $(patsubst apps/$(1)/%.c,$(USER_BUILD)/apps/$(1)/%.o,$(wildcard apps/$(1)/*.c))

# Ports: every folder ports/NAME/ is a program for the full toolchain (mlibc,
# C++; toolchain/, image myos-toolchain). Its .c and .cpp files link into
# NAME.elf. Without that compiler (the plain build image) they are left out.
TOS_CXX    := $(shell command -v x86_64-pc-terminalos-g++ 2>/dev/null)
port_names := $(notdir $(patsubst %/,%,$(wildcard ports/*/)))
ifneq ($(TOS_CXX),)
PORT_ELFS  := $(addprefix $(USER_BUILD)/,$(addsuffix .elf,$(port_names)))
endif
# Tools the toolchain image built (toolchain/Dockerfile): curl, and the CA
# certificates TLS checks servers against (CACERT.PEM, found as /etc/ssl/cert.pem).
TOS_SYSROOT := /opt/terminalos/sysroot
ifneq ($(wildcard $(TOS_SYSROOT)/usr/bin/curl),)
PORT_ELFS  += $(USER_BUILD)/curl.elf $(USER_BUILD)/CACERT.PEM
endif
PORT_FLAGS := -O2 -g0 -Wall -Wextra -Wno-unused-parameter
port_objs   = $(patsubst ports/$(1)/%,$(USER_BUILD)/ports/$(1)/%.o,$(wildcard ports/$(1)/*.c ports/$(1)/*.cpp))

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
          -mgeneral-regs-only \
          -mcmodel=kernel \
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
            -mgeneral-regs-only \
            -mcmodel=kernel \
            -Os \
            -Wall \
            -Wextra \
            -Wno-unused-parameter \
            -ffunction-sections \
            -fdata-sections

ASFLAGS := -f elf64

LDFLAGS := -n -T $(LINKER_LD) --gc-sections

# `make DEBUG=1` -> debug symbols, no optimisation; otherwise -O2.
# -mgeneral-regs-only above keeps the kernel off the SSE registers, which
# interrupts do not save (they belong to the running program). -mcmodel=kernel:
# the kernel runs in the top 2 GiB of the address space (linker.ld).
ifeq ($(DEBUG),1)
CFLAGS += -g -O0
CXXFLAGS += -g -O0
ASFLAGS += -g -F dwarf
else
CFLAGS += -O2
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

# uACPI (third_party/uacpi, MIT): the ACPI interpreter behind drivers/power.c.
UACPI_DIR := third_party/uacpi
uacpi_c   := $(wildcard $(UACPI_DIR)/source/*.c)
uacpi_obj := $(patsubst $(UACPI_DIR)/source/%.c,$(BUILD)/uacpi/%.o,$(uacpi_c))

objects := $(kernel_obj) $(x86_c_obj) $(x86_cpp_obj) $(x86_asm_obj) $(uacpi_obj)

# The kernel image starts at 1 MiB and must end before the page-table pool at 4 MiB.
KERNEL_BASE  := 1048576
KERNEL_LIMIT := 4194304

# ---- Targets --------------------------------------------------------------

.PHONY: all build-x86_64 iso usb run run-iso run-nodisk debug disk size clean help test-net user test test-unit test-assets test-os

.DEFAULT_GOAL := help

help: ## Show this help
	@echo "Terminal OS - make targets:"
	@grep -hE '^[a-zA-Z0-9_-]+:.*## ' $(THIS_MAKEFILE) | \
		awk -F':.*## ' '{printf "  %-14s %s\n", $$1, $$2}'
	@echo ""
	@echo "Options: DEBUG=1  WERROR=1  CROSS=<prefix>  DISK_IMG=<file>  RAMDISK_MB=<n>"

all: build-x86_64 ## Alias for build-x86_64

build-x86_64: $(USB_IMG) size ## Build dist/x86_64/terminalos.img, the USB stick (run inside Docker)

iso: $(KERNEL_ISO) ## Legacy: a GRUB ISO (BIOS + UEFI), for virtual machines without UEFI


# ---- C sources ------------------------------------------------------------

$(BUILD)/kernel/%.o: $(SRC_KERNEL)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP $< -o $@


$(BUILD)/x86_64/%.o: $(SRC_X86)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP $< -o $@


# Third-party code: optimised for size, its own warnings left to upstream.
$(BUILD)/uacpi/%.o: $(UACPI_DIR)/source/%.c
	@mkdir -p $(dir $@)
	$(CC) -c -I $(UACPI_DIR)/include -DUACPI_USE_BUILTIN_STRING -ffreestanding -fno-stack-protector \
	    -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -Os -ffunction-sections -fdata-sections -w \
	    -MMD -MP $< -o $@


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
	    CFLAGS="-ffreestanding -fno-builtin -fno-stack-protector -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -Os -ffunction-sections -fdata-sections" \
	    INCFLAGS="-Isrc -Iinc -I$(abspath buildenv/freestanding/include)" \
	    STATICLIB=lib DLL=no TOOLS=no TESTS=no


$(KERNEL_BIN): $(objects) $(BEARSSL_LIB) $(LINKER_LD)
	@mkdir -p $(DIST)
	$(LD) $(LDFLAGS) -o $@ $(objects) $(BEARSSL_LIB)


# ---- ISO ------------------------------------------------------------------

$(RAMDISK_GZ): scripts/mkramdisk.sh scripts/mkdock.sh $(wildcard $(RAMDISK_DIR)/*) $(USER_ELFS) $(APP_ELFS) $(PORT_ELFS) \
               $(wildcard apps/*/app.cfg) $(THIS_MAKEFILE)
	rm -rf $(RAMDISK_STAGE)
	mkdir -p $(RAMDISK_STAGE)
	cp $(RAMDISK_DIR)/* $(USER_ELFS) $(APP_ELFS) $(PORT_ELFS) $(RAMDISK_STAGE)/
	sh scripts/mkdock.sh apps > $(RAMDISK_STAGE)/DOCK.CFG
	sh scripts/mkramdisk.sh $(RAMDISK_STAGE) $(RAMDISK_MB) $@


# ---- User programs ----------------------------------------------------------

user: $(USER_ELFS) $(APP_ELFS) ## Build the user programs and apps (build/user/*.elf)

# Keep the program objects (make would delete them as intermediates).
.SECONDARY:

$(USER_BUILD)/lib/%.o: user/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP $< -o $@

$(USER_BUILD)/programs/%.o: user/programs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -MMD -MP $< -o $@

# The video player decodes with pl_mpeg (third_party/pl_mpeg, MIT), whose
# sound decoder uses floating point: SSE is allowed here (the kernel saves
# every task's SSE registers).

# Everything else it plays (MP4, MKV, WebM...) is decoded by FFmpeg (LGPL),
# built from the source in the build image by scripts/build-ffmpeg.sh.
FFMPEG_SRC ?= /root/ffmpeg
FFMPEG_OUT  = $(BUILD)/ffmpeg
FFMPEG_LIBS = $(addprefix $(FFMPEG_OUT)/lib/,libavformat.a libavcodec.a libswresample.a libavutil.a)
APP_LIBS_video = $(FFMPEG_LIBS) $(shell $(CC) -print-libgcc-file-name)
$(FFMPEG_OUT)/lib/libavutil.a: scripts/build-ffmpeg.sh | $(USER_BUILD)/libc.a $(USER_BUILD)/crt0.o
	sh scripts/build-ffmpeg.sh $(FFMPEG_SRC) $(FFMPEG_OUT)
$(filter-out %/libavutil.a,$(FFMPEG_LIBS)): $(FFMPEG_OUT)/lib/libavutil.a
$(call app_objs,video): | $(FFMPEG_OUT)/lib/libavutil.a
$(USER_BUILD)/apps/video/%.o: USER_CFLAGS := $(filter-out -mgeneral-regs-only,$(USER_CFLAGS)) -I third_party/pl_mpeg -I $(FFMPEG_OUT)/include -Wno-sign-compare -Wno-unused-function

# stb_image (third_party/stb, public domain) decodes JPEG, GIF, TGA and PSD;
# minimp3 (third_party/minimp3, CC0) decodes MP3 for Music. Both use floating point.
$(USER_BUILD)/lib/stbimage.o: USER_CFLAGS := $(filter-out -mgeneral-regs-only,$(USER_CFLAGS)) -I third_party/stb -w
# The C library's floating point: maths, strtod, printf's %f (programs that
# never use floating point don't run any of it).
$(USER_BUILD)/lib/libm.o $(USER_BUILD)/lib/libc2.o $(USER_BUILD)/lib/stdio.o: USER_CFLAGS := $(filter-out -mgeneral-regs-only,$(USER_CFLAGS)) -fno-math-errno
$(USER_BUILD)/apps/music/%.o: USER_CFLAGS := $(filter-out -mgeneral-regs-only,$(USER_CFLAGS)) -I third_party/minimp3 -Wno-sign-compare -Wno-unused-function

$(USER_BUILD)/crt0.o: user/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(USER_BUILD)/libc.a: $(user_lib_obj)
	$(CROSS)ar rcs $@ $^

$(USER_BUILD)/%.elf: $(USER_BUILD)/programs/%.o $(USER_BUILD)/crt0.o $(USER_BUILD)/libc.a user/user.ld
	$(LD) -T user/user.ld -z max-page-size=4096 -z noexecstack -o $@ $(USER_BUILD)/crt0.o $< $(USER_BUILD)/libc.a

# An app's files: its own folder is on the include path (its headers).
$(USER_BUILD)/apps/%.o: apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -I $(dir $<) -MMD -MP $< -o $@

define APP_RULE
$(USER_BUILD)/$(1).elf: $(call app_objs,$(1)) $$(APP_LIBS_$(1)) $(USER_BUILD)/crt0.o $(USER_BUILD)/libc.a user/user.ld
	$$(LD) -T user/user.ld -z max-page-size=4096 -z noexecstack -o $$@ $(USER_BUILD)/crt0.o $(call app_objs,$(1)) $$(APP_LIBS_$(1)) $(USER_BUILD)/libc.a
endef
$(foreach a,$(app_names),$(eval $(call APP_RULE,$(a))))

# Ports (ports/NAME/, the full toolchain): see PORT_RULE below.

$(USER_BUILD)/ports/%.cpp.o: ports/%.cpp
	@mkdir -p $(dir $@)
	x86_64-pc-terminalos-g++ -std=gnu++23 $(PORT_FLAGS) -MMD -MP -c $< -o $@

$(USER_BUILD)/ports/%.c.o: ports/%.c
	@mkdir -p $(dir $@)
	x86_64-pc-terminalos-gcc $(PORT_FLAGS) -MMD -MP -c $< -o $@

define PORT_RULE
$(USER_BUILD)/$(1).elf: $(call port_objs,$(1))
	x86_64-pc-terminalos-g++ -o $$@ $$^
	x86_64-pc-terminalos-strip $$@
endef
$(foreach p,$(port_names),$(eval $(call PORT_RULE,$(p))))

$(USER_BUILD)/curl.elf: $(TOS_SYSROOT)/usr/bin/curl
	@mkdir -p $(dir $@)
	x86_64-pc-terminalos-strip -o $@ $<

$(USER_BUILD)/CACERT.PEM: /opt/terminalos/share/cacert.pem
	@mkdir -p $(dir $@)
	cp $< $@


$(KERNEL_ISO): $(KERNEL_BIN) $(RAMDISK_GZ) $(ISO_SRC)/boot/grub/grub.cfg
	rm -rf $(ISO_STAGE)
	mkdir -p $(ISO_STAGE)
	cp -r $(ISO_SRC)/. $(ISO_STAGE)/
	mkdir -p $(ISO_STAGE)/boot
	cp $(KERNEL_BIN) $(ISO_STAGE)/boot/kernel.bin
	cp $(RAMDISK_GZ) $(ISO_STAGE)/boot/ramdisk.img.gz
	grub-mkrescue -o $@ $(ISO_STAGE)   # BIOS + UEFI (all installed GRUB platforms)


# ---- The boot loader and the USB stick image ------------------------------------
#
# The loader is a UEFI program (PE/COFF), so MinGW builds it, not the ELF
# cross compiler. terminalos.img is what goes on a USB stick (written raw).

$(LOADER_EFI): $(wildcard loader/*.c loader/*.h)
	@mkdir -p $(dir $@)
	$(LOADER_CC) -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-stack-check -mno-stack-arg-probe \
	    -mno-red-zone -mgeneral-regs-only -fshort-wchar -Wall -Wextra -nostdlib \
	    -Wl,--subsystem,10 -Wl,-e,efi_main -o $@ loader/main.c loader/inflate.c

$(USB_IMG): $(LOADER_EFI) $(KERNEL_BIN) $(RAMDISK_GZ) scripts/mkusbimg.sh
	@mkdir -p $(dir $@)
	sh scripts/mkusbimg.sh $@ $(LOADER_EFI) $(KERNEL_BIN) $(RAMDISK_GZ)

usb: $(USB_IMG) ## Build dist/x86_64/terminalos.img (the USB stick)

# ---- Size -----------------------------------------------------------------

size: $(KERNEL_BIN) ## Print kernel size and remaining headroom below the heap
	@$(CROSS)size $(KERNEL_BIN)
	@end=$$($(CROSS)nm $(KERNEL_BIN) | \
		awk '$$3=="kernel_phys_end"{print "0x"$$1}'); \
	used=$$(( end - $(KERNEL_BASE) )); \
	free=$$(( $(KERNEL_LIMIT) - end )); \
	echo "kernel image: $$used bytes mapped, $$free bytes of headroom (limit $$(( $(KERNEL_LIMIT) - $(KERNEL_BASE) )))"


# ---- Host-side helpers ----------------------------------------------------

disk: ## Create a 32 MB FAT32 test disk (needs dosfstools + mtools)
	sh scripts/mkdisk.sh $(DISK_IMG)


run: ## Boot the USB stick image in QEMU (UEFI, host-side; build first)
	@test -f $(USB_IMG) || { \
		echo "No $(USB_IMG): run 'make build-x86_64' (in Docker) first"; \
		exit 1; \
	}
	$(QEMU) -cpu max -m 1G -drive if=pflash,format=raw,readonly=on,file="$(OVMF)" \
	    -drive if=virtio,file=$(USB_IMG),format=raw,snapshot=on \
	    -device rtl8139,netdev=n0 -netdev user,id=n0 -serial stdio


run-iso: ## Legacy: boot the GRUB ISO with disk + NIC (host-side; `make iso` first)
	@test -f $(KERNEL_ISO) || { \
		echo "No $(KERNEL_ISO): run 'make iso' (in Docker) first"; \
		exit 1; \
	}
	$(QEMU) -cpu max -m 512M -cdrom $(KERNEL_ISO) \
	    -drive file=$(DISK_IMG),format=raw,index=0,media=disk -boot d \
	    -device rtl8139,netdev=n0 -netdev user,id=n0 -serial stdio


run-nodisk: ## Legacy: boot the GRUB ISO without a disk (host-side)
	@test -f $(KERNEL_ISO) || { \
		echo "No $(KERNEL_ISO): run 'make iso' (in Docker) first"; \
		exit 1; \
	}
	$(QEMU) -cpu max -m 512M -cdrom $(KERNEL_ISO) -boot d -serial stdio


debug: ## Boot paused with a gdb server on :1234
	@test -f $(KERNEL_ISO) || { \
		echo "No $(KERNEL_ISO): run 'make DEBUG=1 iso' (in Docker) first"; \
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
	$(NOASLR) $(BUILD)/test/net_host_test

test-tcp: ## Unit-test TCP on the host against a simulated peer (loss, reordering)
	mkdir -p $(BUILD)/test
	gcc -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined -I src/intf \
	    tests/tcp_host_test.c $(SRC_X86)/net/tcp.c -o $(BUILD)/test/tcp_host_test
	$(NOASLR) $(BUILD)/test/tcp_host_test

test-wpa: ## Unit-test the WPA2 handshake on the host (IEEE/RFC vectors, simulated AP)
	mkdir -p $(BUILD)/test
	$(MAKE) -C $(BEARSSL_DIR) -s lib >/dev/null      # a host build of BearSSL
	gcc -g -O1 -Wall -I src/intf -I $(BEARSSL_DIR)/inc tests/wpa_host_test.c $(SRC_X86)/net/wpa.c \
	    $(BEARSSL_DIR)/build/libbearssl.a -o $(BUILD)/test/wpa_host_test
	$(NOASLR) $(BUILD)/test/wpa_host_test

test-unit: test-net test-tcp test-wpa ## All the host-side unit tests

test-assets: $(USB_IMG) $(KERNEL_ISO) scripts/mktestdisks.sh ## Disk images and a video for the system tests
	sh scripts/mktestdisks.sh $(BUILD)/test

test: test-unit test-assets ## Unit tests and what the system tests need (in Docker); then `make test-os` on the host
	@echo "Now on the host: make test-os   (or: python tests/qemu/run_tests.py)"

test-os: ## System tests: boot in QEMU, type commands, check the output (host-side; `make test` first)
	python tests/qemu/run_tests.py


# ---- Clean ----------------------------------------------------------------

clean: ## Remove build/ and dist/
	rm -rf $(BUILD) dist


# ---- Header dependency tracking -------------------------------------------

-include $(foreach p,$(port_names),$(patsubst %.o,%.d,$(call port_objs,$(p))))
-include $(foreach a,$(app_names),$(patsubst %.o,%.d,$(call app_objs,$(a))))
-include $(objects:.o=.d) $(user_lib_obj:.o=.d) $(patsubst %.elf,%.d,$(subst $(USER_BUILD)/,$(USER_BUILD)/programs/,$(USER_ELFS)))

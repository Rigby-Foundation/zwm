# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# zwm: the window server for sic, and libzwm, what programs talk to it with.
# Optional: builds against the sysroot like any ZAE program and installs
# into $(SYSROOT)/rootfs, which ZAE overlays onto the initrd.

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  ?= $(firstword $(foreach f,lld lld@21 lld@20 llvm,$(if $(wildcard $(shell brew --prefix $(f) 2>/dev/null)/bin/ld.lld),$(shell brew --prefix $(f)),)))
ifeq ($(origin CC),default)
CC := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/clang,clang)
endif
ifeq ($(origin LD),default)
LD := $(if $(LLD_PREFIX),$(LLD_PREFIX)/bin/ld.lld,ld.lld)
endif
ifeq ($(origin AR),default)
AR := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/llvm-ar,ar)
endif

SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
LIBC    := $(SYSROOT)/usr
ARCH    ?= x86_64
BUILD   := build/$(ARCH)

ifeq ($(ARCH),powerpc)
TARGET := powerpc-linux-musl
ARCH_CFLAGS := -mcpu=7450 -maltivec -fno-pic -fno-pie
IMAGE_BASE := 0x10000000
LD_EMUL := -m elf32ppc
else ifeq ($(ARCH),aarch64)
TARGET := aarch64-linux-musl
ARCH_CFLAGS := -fPIE
IMAGE_BASE := 0x8000000000
LD_EMUL := -m aarch64elf
else
TARGET := x86_64-linux-musl
ARCH_CFLAGS := -fPIE
IMAGE_BASE := 0x8000000000
LD_EMUL :=
endif

CFLAGS  := --target=$(TARGET) -std=c11 -nostdinc -isystem $(LIBC)/include -Iinclude \
           $(ARCH_CFLAGS) -fno-stack-protector -fno-asynchronous-unwind-tables \
           -O2 -g -Wall -Wextra -D_GNU_SOURCE
LDFLAGS := $(LD_EMUL) -static -nostdlib --image-base=$(IMAGE_BASE) -z max-page-size=0x1000 -z noexecstack \
           --defsym=_DYNAMIC=$(IMAGE_BASE)
CRT_BEGIN := $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o
CRT_END   := $(LIBC)/lib/crtn.o
stamp-osabi = printf '\123' | dd of=$(1) bs=1 seek=7 count=1 conv=notrunc status=none

LIB_OBJS := $(patsubst lib/%.c,$(BUILD)/lib/%.o,$(wildcard lib/*.c))
LIB      := $(BUILD)/libzwm.a
SERVER   := $(BUILD)/zwm

.PHONY: all install clean
all: $(LIB) $(SERVER)

$(BUILD)/%.o: %.c include/zwm.h $(wildcard server/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

# Composing on the GPU needs zgl, which is built after zwm (it links
# libzwm): the first build composes in software, `make install` again once
# zgl is in the sysroot and zwm picks it up, with its backends: virgl
# (libzgl) and the Adreno (libadreno, inside libvirgl).
ZGL_LIBS := $(if $(wildcard $(LIBC)/lib/libzgl.a),$(LIBC)/lib/libzgl.a $(LIBC)/lib/libvirgl.a)
HWCOMP   := $(if $(ZGL_LIBS),$(BUILD)/server/hwcomp.o $(BUILD)/server/hwcomp_gl.o $(BUILD)/server/hwcomp_adreno.o,$(BUILD)/server/hwcomp_none.o)

$(BUILD)/server/hwcomp.o: server/hwcomp.c server/hwcomp.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DHWCOMP_ZGL -c $< -o $@

$(BUILD)/server/hwcomp_none.o: server/hwcomp.c server/hwcomp.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(SERVER): $(BUILD)/server/zwm.o $(HWCOMP) $(LIB) $(ZGL_LIBS)
	$(LD) $(LDFLAGS) -o $@ $(CRT_BEGIN) $< $(HWCOMP) $(ZGL_LIBS) $(LIB) $(LIBC)/lib/libc.a $(wildcard $(LIBC)/lib/libcompiler_rt.a) $(CRT_END)
	@$(call stamp-osabi,$@)

install: all
	@mkdir -p $(SYSROOT)/rootfs/bin $(SYSROOT)/usr/include $(SYSROOT)/usr/lib
	cp $(SERVER) $(SYSROOT)/rootfs/bin/zwm
	cp include/zwm.h $(SYSROOT)/usr/include/
	cp $(LIB) $(SYSROOT)/usr/lib/libzwm.a
	@mkdir -p $(SYSROOT)/rootfs/usr/share/fonts
	cp fonts/*.ttf fonts/OFL-*.txt $(SYSROOT)/rootfs/usr/share/fonts/
	@echo "installed into $(SYSROOT)"

clean:
	rm -rf build

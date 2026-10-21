# OpenRSP bridge firmware.
#
#   make [VARIANT=common|rsp1|rspduo] [DEV=1] [O=<outdir>]   one image: $(O)/fw_<variant>.bin
#   make all [DEV=1] [O=<outdir>]                            all three + manifest.sha256
#   make check [O=<outdir>]                                  static gates on release and dev
#                                                            images, two clean builds byte-identical
#   make own                                                 rebuild the committed release images
#                                                            in firmware/own/ (+ manifest.sha256)
#   make clean [O=<outdir>]
#
# Needs SDCC 4.2 (sdcc, sdas8051) and objcopy (binutils). Output defaults to ./build (never
# firmware/*.bin).

VARIANT ?= common
DEV     ?= 0
O       ?= build

SRCDIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO    := $(abspath $(SRCDIR)/../..)
PY      := python3 -I
OBJCOPY ?= objcopy
SDCC    := sdcc
SDAS    := sdas8051
# Static gates on each image (repository only; the linker enforces the 0x1800 code size anyway).
FWCHECK := $(REPO)/tools/fw8051/fwcheck.py
GATES_MSG := $(if $(wildcard $(FWCHECK)),static gates passed$(strip ,) )
# Optional extra checks for `make check`, run after the reproducibility builds: a checks.mk in a
# subdirectory may define CHECK_EXTRA (recipe lines; the release images are in $(O)/repro_a) and
# CHECK_EXTRA_MSG (appended to the summary line).
CHECK_EXTRA :=
CHECK_EXTRA_MSG :=
-include $(wildcard $(SRCDIR)/*/checks.mk)

VARIANTS := common rsp1 rspduo
C_SRCS   := main.c bridge.c spi.c boot_record.c desc.c usb_isr.c ep0.c std_req.c vendor.c \
            stream.c
ASM_SRCS := isr_int0.asm delay.asm

CFLAGS  := -mmcs51 --model-small --opt-code-size --std-sdcc11 -I$(SRCDIR)/include
# 0x1800: the ROM loader takes an image of up to 6 KiB through vendor request 0x44, as SDRplay's
# driver uploading its firmware shows on the USB bus.
LDFLAGS := -mmcs51 --model-small --code-loc 0x0000 --code-size 0x1800 \
           --xram-loc 0x1800 --xram-size 0x0100 --iram-size 0x80

ifeq ($(DEV),1)
SUFFIX  := _dev
DEFS_DEV := -DFW_DEV=1
CHECK_DEV := --dev
else
SUFFIX  :=
DEFS_DEV :=
CHECK_DEV :=
endif

VUP      = $(shell echo $(1) | tr a-z A-Z)
OBJDIR  := $(O)/obj_$(VARIANT)$(SUFFIX)
IMAGE   := $(O)/fw_$(VARIANT)$(SUFFIX).bin
DEFS    := -DFW_VARIANT_$(call VUP,$(VARIANT)) $(DEFS_DEV)
OBJS    := $(addprefix $(OBJDIR)/,$(C_SRCS:.c=.rel) $(ASM_SRCS:.asm=.rel))
HDRS    := $(wildcard $(SRCDIR)/include/*.h)

.PHONY: image all check own clean
.DEFAULT_GOAL := image

image: $(IMAGE)

$(OBJDIR)/%.rel: $(SRCDIR)/%.c $(HDRS) | $(OBJDIR)
	$(SDCC) $(CFLAGS) $(DEFS) -c $< -o $@

$(OBJDIR)/%.rel: $(SRCDIR)/%.asm | $(OBJDIR)
	$(SDAS) -plosgff -o $@ $<

$(OBJDIR):
	mkdir -p $@

$(IMAGE): $(OBJS)
	$(SDCC) $(LDFLAGS) $(OBJS) -o $(OBJDIR)/fw.ihx
	$(OBJCOPY) -I ihex -O binary --gap-fill 0x00 $(OBJDIR)/fw.ihx $(OBJDIR)/fw.bin
	$(if $(wildcard $(FWCHECK)),$(PY) $(FWCHECK) $(OBJDIR)/fw.bin $(OBJDIR)/fw.map $(CHECK_DEV) --variant $(VARIANT))
	cp $(OBJDIR)/fw.map $(O)/fw_$(VARIANT)$(SUFFIX).map
	cp $(OBJDIR)/fw.mem $(O)/fw_$(VARIANT)$(SUFFIX).mem
	cp $(OBJDIR)/fw.bin $@

all:
	@for v in $(VARIANTS); do $(MAKE) --no-print-directory -f $(SRCDIR)/Makefile \
	    VARIANT=$$v DEV=$(DEV) O=$(O) image || exit 1; done
	cd $(O) && sha256sum $(foreach v,$(VARIANTS),fw_$(v)$(SUFFIX).bin) > manifest$(SUFFIX).sha256
	@cat $(O)/manifest$(SUFFIX).sha256

# Static gates (fwcheck on every image) on release + dev images; two clean builds must be
# byte-identical.
check:
	rm -rf $(O)/repro_a $(O)/repro_b
	$(MAKE) --no-print-directory -f $(SRCDIR)/Makefile all O=$(O)/repro_a
	$(MAKE) --no-print-directory -f $(SRCDIR)/Makefile all O=$(O)/repro_a DEV=1
	$(MAKE) --no-print-directory -f $(SRCDIR)/Makefile all O=$(O)/repro_b
	$(MAKE) --no-print-directory -f $(SRCDIR)/Makefile all O=$(O)/repro_b DEV=1
	cmp $(O)/repro_a/manifest.sha256 $(O)/repro_b/manifest.sha256
	cmp $(O)/repro_a/manifest_dev.sha256 $(O)/repro_b/manifest_dev.sha256
	$(CHECK_EXTRA)
	@echo "check: $(GATES_MSG)two clean builds byte-identical$(CHECK_EXTRA_MSG)"

# Rebuild the release images into firmware/own (CMake installs them to <firmware dir>/own).
own:
	rm -rf $(O)/own_build
	$(MAKE) --no-print-directory -f $(SRCDIR)/Makefile all O=$(O)/own_build
	mkdir -p $(REPO)/firmware/own
	cp $(foreach v,$(VARIANTS),$(O)/own_build/fw_$(v).bin) $(O)/own_build/manifest.sha256 $(REPO)/firmware/own/
	@echo "own: firmware/own updated"

clean:
	rm -rf $(O)

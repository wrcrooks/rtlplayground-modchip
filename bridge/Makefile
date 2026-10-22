# Mod-chip bridge firmware. Run from the repository root:
#
#   make -f modchip/bridge/Makefile MACHINE=SWGT024_V2_0_UNMANAGED LAYOUT=compact
#
# It shares the chip-level modules with RTLPlayground and leaves out the
# network stack, the web UI, the command line and the protocol handlers,
# which run on the host. The image has the layout of an RTLPlayground image,
# so the boot ROM, a programmer and the staged update path all treat it the
# same way. A 512 KiB flash needs LAYOUT=compact for staged updates.

ifeq ($(LAYOUT),compact)
IMAGESIZE = 258048
DEFAULT_CONFIG_LOCATION = 253952
FLASH_512K = 524288
else ifeq ($(LAYOUT),)
IMAGESIZE = 524288
DEFAULT_CONFIG_LOCATION = 454656
else
$(error LAYOUT must be unset or "compact")
endif

ifeq ($(MACHINE),)
$(error MACHINE is not set, e.g. MACHINE=SWGT024_V2_0_UNMANAGED)
endif

ifeq ($(origin CC),default)
CC = sdcc
endif
ASM ?= sdas8051
AFLAGS = -plosgff
CC_FLAGS = -mmcs51 -I. -Imachine -Ihttpd -Iuip -Imodchip/bridge
CC_FLAGS += -DMACHINE_$(MACHINE) -DMACHINE_NAME=$(MACHINE)
ifeq ($(LAYOUT),compact)
CC_FLAGS += -DLAYOUT_COMPACT
endif
ifeq ($(CI),1)
CC_FLAGS += --Werror
endif
ifneq ($(LINK_TH1),)
CC_FLAGS += -DLINK_TH1=$(LINK_TH1)
endif

BUILDDIR = output/bridge-$(MACHINE)$(if $(filter compact,$(LAYOUT)),-compact)
GIT_VERSION := $(shell git rev-parse --short HEAD)$(if $(shell git status --porcelain --untracked-files=no),-dirty)
TARGET = $(BUILDDIR)/bridge-$(GIT_VERSION)-$(MACHINE)$(if $(filter compact,$(LAYOUT)),-compact).bin

SRCS = \
	modchip/bridge/bridge.c \
	modchip/bridge/hal.c \
	modchip/bridge/link.c \
	machine/$(MACHINE).c \
	machine_init.c \
	boot.c \
	sfp.c \
	rtl837x_flash.c \
	rtl837x_init.c \
	rtl837x_leds.c \
	rtl837x_nic.c \
	rtl837x_phy.c \
	rtl837x_pins.c \
	rtl837x_port.c \
	rtl837x_trap.c

OBJS = $(addprefix $(BUILDDIR)/,$(notdir $(SRCS:.c=.rel)))
VPATH = modchip/bridge machine .

all: $(TARGET)

# The version header and the image tools come from the main build
version.h:
	$(MAKE) version.h

tools/output/imagebuilder tools/output/fileadder tools/output/crc_calculator:
	$(MAKE) -C tools

$(BUILDDIR):
	mkdir -p $@

$(BUILDDIR)/%.rel: %.c version.h | $(BUILDDIR)
	$(CC) -MMD $(CC_FLAGS) -o $@ -c $<

$(BUILDDIR)/%.rel: %.asm | $(BUILDDIR)
	$(ASM) $(AFLAGS) -o $@ $<

$(BUILDDIR)/bridge.ihx: $(OBJS) $(BUILDDIR)/crtbank.rel $(BUILDDIR)/crc16.rel
	$(CC) $(CC_FLAGS) --xram-size 49151 -Wl-bHOME=0x00000 -Wl-bBANK1=0x14000 -Wl-bBANK2=0x24000 -Wl-bBANK3=0x34000 -Wl-r -o $@ $^

$(BUILDDIR)/bridge.img: $(BUILDDIR)/bridge.ihx
	objcopy --input-target=ihex -O binary $< $@

# The default config only sizes the image to IMAGESIZE; the bridge never
# reads it, the host keeps the configuration
$(TARGET): $(BUILDDIR)/bridge.img tools/output/imagebuilder tools/output/fileadder tools/output/crc_calculator
	rm -f $@
	tools/output/imagebuilder -i $< $@
	tools/output/fileadder -a $(DEFAULT_CONFIG_LOCATION) -s $(IMAGESIZE) -d config.txt $@
	tools/output/crc_calculator -u $@
ifeq ($(LAYOUT),compact)
	{ cat $@; head -c $$(( $(FLASH_512K) - $(IMAGESIZE) )) /dev/zero | tr '\000' '\377'; } \
		> $(@:.bin=-flash512k.bin)
endif

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean

-include $(OBJS:.rel=.d)

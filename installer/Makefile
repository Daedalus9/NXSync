ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := NXSyncInstaller
BUILD       := build
SOURCES     := source
INCLUDES    := include ../include
ROMFS       := romfs

APP_TITLE   := NXSync Installer
APP_AUTHOR  := Daedalus9
APP_VERSION := 0.1.13-rc2

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -D__SWITCH__
CXXFLAGS    := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -Wl,-Map,$(notdir $*.map)
LIBS        := -lnx
LIBDIRS     := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT    := $(CURDIR)/$(TARGET)
export TOPDIR    := $(CURDIR)
export APP_ICON  := $(CURDIR)/../assets/icons/installer.jpg
export VPATH     := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR   := $(CURDIR)/$(BUILD)

CFILES           := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES         := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES           := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD        := $(CXX)
export OFILES    := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE   := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)
export LIBPATHS  := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS  += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp
ifneq ($(ROMFS),)
export NROFLAGS  += --romfsdir=$(CURDIR)/$(ROMFS)
export ROMFS_FILES := $(shell find $(CURDIR)/$(ROMFS) -type f)
endif

.PHONY: all clean $(BUILD)

all: $(BUILD)

$(BUILD):
	@test -f romfs/payloads/manifest.ini || \
	  (echo "Missing installer payload manifest. Run scripts/stage-installer-payloads.py." && false)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

.PHONY: all
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro
# FORCE also catches removed files and restored timestamps. Packaging independently
# verifies every embedded byte against the current staging tree.
.PHONY: FORCE_ROMFS
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(APP_ICON) $(ROMFS_FILES) FORCE_ROMFS
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif

.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := NXSync Overlay
APP_AUTHOR  := Daedalus9
APP_VERSION := 0.3.22-rc1
TARGET      := NXSync
BUILD       := build
SOURCES     := source ../source
INCLUDES    := ../include
NO_ICON     := 1

ifeq ($(BUILD),$(notdir $(CURDIR)))
include $(TOPDIR)/lib/libultrahand/ultrahand.mk
else
override TOPDIR := $(CURDIR)
include $(CURDIR)/lib/libultrahand/ultrahand.mk
endif

ARCH        := -march=armv8-a+simd+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -O2 -ffunction-sections -fdata-sections -flto \
               -fuse-linker-plugin -fomit-frame-pointer \
               -fno-strict-aliasing $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -D__SWITCH__ -DAPP_VERSION="\"$(APP_VERSION)\"" \
               -DUI_OVERRIDE_PATH="\"/config/NXSync/overlay-ui/\""
CXXFLAGS    := $(CFLAGS) -std=gnu++20 -fno-exceptions -fno-rtti \
               -fno-unwind-tables -fno-asynchronous-unwind-tables
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -Wl,-Map,$(notdir $*.map) -Wl,--as-needed -Wl,--gc-sections -flto
LIBS        := -lcurl -lz -lminizip -lmbedtls -lmbedx509 -lmbedcrypto -lnx
LIBDIRS     := $(PORTLIBS) $(LIBNX)

SHARED_CPPFILES := atomic_file.cpp launch_protocol.cpp overlay_catalog.cpp preflight_protocol.cpp \
                   sysmodule_config.cpp sysmodule_status.cpp

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(CURDIR)/source $(CURDIR)/../source \
                  $(foreach dir,$(filter-out source ../source,$(SOURCES)),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES        := $(notdir $(wildcard source/*.cpp)) $(SHARED_CPPFILES) \
                   $(foreach dir,$(filter-out source ../source,$(SOURCES)),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD       := $(CXX)
export OFILES   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: all clean dist $(BUILD)

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@mkdir -p dist/switch/.overlays
	@cp $(TARGET).ovl dist/switch/.overlays/NXSync.ovl

dist:
	@echo "Distribute the verified installer and corresponding-source assets using scripts/package-installer.py."
	@false

clean:
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp \
	        $(TARGET).elf dist NXSync-overlay-$(APP_VERSION).zip

else

.PHONY: all
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@printf 'ULTR' >> $@
	@echo "built ... $(notdir $(OUTPUT).ovl)"

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif

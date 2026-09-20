ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set. Run this Makefile from a devkitPro shell")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := NXSync
BUILD       := build
SOURCES     := source third_party/qrcodegen overlay/lib/libultrahand/common
INCLUDES    := include third_party/qrcodegen overlay/lib/libultrahand/common

APP_TITLE   := NXSync
APP_AUTHOR  := Daedalus9
APP_VERSION := 0.30.12-rc2

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -D__SWITCH__
CXXFLAGS    := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -Wl,-Map,$(notdir $*.map)
LIBS        := -lminizip -lcurl -lmbedcrypto -lSDL2_ttf -lSDL2 \
               -lEGL -lglapi -ldrm_nouveau \
               -lharfbuzz -lfreetype -lbz2 -lpng16 \
               -lz -lpthread -lm -lnx

LIBDIRS     := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT    := $(CURDIR)/$(TARGET)
export TOPDIR    := $(CURDIR)
export APP_ICON  := $(CURDIR)/assets/icons/nxsync.jpg
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

.PHONY: all clean nro components installer sysmodule cloud-worker overlay \
	dependency-licenses clean-all $(BUILD)

all: $(BUILD)

nro: all

sysmodule:
	@$(MAKE) --no-print-directory -C sysmodule

cloud-worker:
	@$(MAKE) --no-print-directory -C cloud-worker

overlay:
	@$(MAKE) --no-print-directory -C overlay

dependency-licenses:
	@bash scripts/stage-dependency-licenses.sh

installer: dependency-licenses
	@$(MAKE) --no-print-directory -C installer

components: dependency-licenses all sysmodule cloud-worker overlay

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

clean-all: clean
	@$(MAKE) --no-print-directory -C sysmodule clean
	@$(MAKE) --no-print-directory -C cloud-worker clean
	@$(MAKE) --no-print-directory -C overlay clean
	@$(MAKE) --no-print-directory -C installer clean
	@rm -fr build-tests dist

else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(APP_ICON)
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif

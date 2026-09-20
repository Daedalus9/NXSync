.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := nxsync-sysmodule
BUILD       := build
SOURCES     := source
INCLUDES    := include ../include
CONFIG_JSON := nxsync-sysmodule.json
TITLE_ID    := 4200000000004E58

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -Os -flto -ffunction-sections \
               -fno-unwind-tables -fno-asynchronous-unwind-tables \
               $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -D__SWITCH__
CXXFLAGS    := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions \
               -DNXSYNC_BACKUP_BUFFER_SIZE=65536 -DNXSYNC_ZIP_MEM_LEVEL=1 \
               -DNXSYNC_ZIP_STORE_ONLY=1
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -flto -Wl,-Map,$(notdir $*.map)
LIBS        := -lnx
LIBDIRS     := $(PORTLIBS) $(LIBNX)

SHARED_CPPFILES := application_lifecycle.cpp atomic_file.cpp backup_request_probe.cpp backup_request_queue.cpp \
                   cloud_queue.cpp display_text.cpp launch_protocol.cpp \
                   local_resolution.cpp overlay_catalog.cpp preflight_protocol.cpp storage_environment.cpp sysmodule_config.cpp \
                   sysmodule_status.cpp ultrahand_notification.cpp

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT    := $(CURDIR)/$(TARGET)
export TOPDIR    := $(CURDIR)
export VPATH     := $(CURDIR)/source $(CURDIR)/../source
export DEPSDIR   := $(CURDIR)/$(BUILD)
export APP_JSON  := $(CURDIR)/$(CONFIG_JSON)

CPPFILES         := $(notdir $(wildcard source/*.cpp)) $(SHARED_CPPFILES)
export LD        := $(CXX)
export OFILES    := $(CPPFILES:.cpp=.o)
export INCLUDE   := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)
export LIBPATHS  := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean package $(BUILD)

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

package: all
	@mkdir -p dist/atmosphere/contents/$(TITLE_ID)/flags
	@mkdir -p dist/config/NXSync
	@cp $(TARGET).nsp dist/atmosphere/contents/$(TITLE_ID)/exefs.nsp
	@touch dist/atmosphere/contents/$(TITLE_ID)/flags/boot2.flag
	@cp nxsync-sysmodule.ini dist/config/NXSync/sysmodule.ini

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm \
	        $(TARGET).elf dist

else

.PHONY: all
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nsp
$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif

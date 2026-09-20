.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := nxsync-cloud-worker
BUILD       := build
SOURCES     := source
INCLUDES    := include ../include
CONFIG_JSON := nxsync-cloud-worker.json
TITLE_ID    := 4200000000004E59

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -D__SWITCH__
CXXFLAGS    := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -Wl,-Map,$(notdir $*.map)
LIBS        := -lcurl -lmbedcrypto -lminizip -lz -lpthread -lm -lnx
LIBDIRS     := $(PORTLIBS) $(LIBNX)

SHARED_CPPFILES := app_config.cpp atomic_file.cpp backup_format.cpp backup_manager.cpp backup_request_queue.cpp backup_state.cpp cloud_index.cpp \
                   cloud_queue.cpp cloud_worker_status.cpp device_identity.cpp \
                   credential_crypto.cpp display_text.cpp game_version.cpp headless_backup.cpp launch_protocol.cpp nextcloud_client.cpp nextcloud_paths.cpp \
                   local_resolution.cpp preflight.cpp preflight_protocol.cpp remote_layout.cpp retention.cpp \
                   restore_manager.cpp revision_compare.cpp revision_lineage.cpp save_catalog.cpp \
                   save_types.cpp storage_environment.cpp sync_engine.cpp ultrahand_notification.cpp

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
	@mkdir -p dist/atmosphere/contents/$(TITLE_ID)
	@cp $(TARGET).nsp dist/atmosphere/contents/$(TITLE_ID)/exefs.nsp

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

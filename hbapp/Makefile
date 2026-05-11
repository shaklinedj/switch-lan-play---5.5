#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET   : name of the output binary (without extension)
# BUILD    : directory where object files go
# SOURCES  : list of directories containing source code
# INCLUDES : list of directories containing header files
# ROMFS    : directory for RomFS data (optional; leave blank if not used)
#
# OUTPUT must contain the icon.jpg for the homebrew menu entry.
#---------------------------------------------------------------------------------
TARGET      := lanplay-setup
BUILD       := build
SOURCES     := source
INCLUDES    := source
ROMFS       :=

APP_TITLE   := LanPlay Setup
APP_AUTHOR  := switch-lan-play
APP_VERSION := 1.6

ICON := icon.jpg

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS      := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -D__SWITCH__ -Wno-stringop-truncation

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS     := -g $(ARCH)

LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -Wl,-Map,$(notdir $*.map)

LIBS        := -lnx

#---------------------------------------------------------------------------------
LIBDIRS     := $(PORTLIBS) $(LIBNX)
#---------------------------------------------------------------------------------

ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD   := $(CXX)

export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_SRC)
export HFILES_BIN :=

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-isystem $(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_TITLE
export APP_AUTHOR
export APP_VERSION
export ICON

.PHONY: $(BUILD) clean

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ --nacp=$(OUTPUT).nacp $(if $(ICON),--icon=$(TOPDIR)/$(ICON),)

$(OUTPUT).nacp:
	@nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

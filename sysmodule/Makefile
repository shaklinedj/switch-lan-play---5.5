#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET    : name of the output binary
# BUILD     : directory where object files are placed
# SOURCES   : list of directories containing source code
# DATA      : list of directories containing data files
# INCLUDES  : list of directories containing header files
#---------------------------------------------------------------------------------
TARGET      := lan_play_sysmodule
BUILD       := build
SOURCES     := source source/ipv4
DATA        := data
INCLUDES    := source
ROMFS       :=

TITLE_ID    := 42000000000000B1

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS      := -g -Wall -O2 -ffunction-sections \
               $(ARCH) $(DEFINES) \
               -DLANPLAY_LITTLE_ENDIAN \
               -D__SWITCH__

CFLAGS      += $(INCLUDE) -D__SWITCH__

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS     := -g $(ARCH)

LDFLAGS     =  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -Wl,-Map,$(notdir $*.map)

LIBS        := -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries (top level: include/ and lib/)
#---------------------------------------------------------------------------------
LIBDIRS     := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# No need to edit below this line
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD       := $(CXX)

export OFILES_BIN   := $(addsuffix .o,$(BINFILES))
export OFILES_SRC   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-isystem $(dir)/include) \
                   -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all atmosphere

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# Pack output into Atmosphere directory layout
atmosphere: all
	@mkdir -p atmosphere/contents/$(TITLE_ID)/flags
	@mkdir -p atmosphere/contents/$(TITLE_ID)/exefs
	@cp $(TARGET).nso atmosphere/contents/$(TITLE_ID)/exefs/main
	@if [ -f final_v1.13/atmosphere/contents/$(TITLE_ID)/exefs/main.npdm ]; then \
	    cp final_v1.13/atmosphere/contents/$(TITLE_ID)/exefs/main.npdm atmosphere/contents/$(TITLE_ID)/exefs/main.npdm; \
	    echo "  [OK] NPDM copied from final_v1.13 stable package"; \
	elif [ -f atmosphere/contents/$(TITLE_ID)/exefs/main.npdm ]; then \
	    echo "  [OK] Reusing existing NPDM (stable)"; \
	elif command -v npdmtool >/dev/null 2>&1; then \
	    npdmtool config/main.json atmosphere/contents/$(TITLE_ID)/exefs/main.npdm; \
	    echo "  [OK] NPDM compiled"; \
	else \
	    echo "  [!] npdmtool not found and no stable NPDM available"; \
	fi
	@if command -v build_pfs0 >/dev/null 2>&1; then \
	    build_pfs0 atmosphere/contents/$(TITLE_ID)/exefs atmosphere/contents/$(TITLE_ID)/exefs.nsp; \
	    echo "  [OK] exefs.nsp generated"; \
	else \
	    echo "  [!] build_pfs0 not found — exefs.nsp not generated"; \
	fi
	@touch atmosphere/contents/$(TITLE_ID)/flags/boot2.flag
	@echo ""
	@echo "==> Atmosphere package ready in atmosphere/"
	@echo "    Copy the 'atmosphere/' folder to the root of your Switch SD card."

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nso $(TARGET).nsp

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all: $(OUTPUT).nso

$(OUTPUT).nso: $(OUTPUT).elf
	@echo "  NSO   $(notdir $@)"
	@elf2nso $< $@

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

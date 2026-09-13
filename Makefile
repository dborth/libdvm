#-------------------------------------------------------------------------------
.SUFFIXES:
#-------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

include $(DEVKITPRO)/wut/share/wut_rules

export VER_MAJOR	:=	1
export VER_MINOR	:=	0
export VER_PATCH	:=	0

VERSION	:=	$(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)

LIBNTFS := libntfs

#-------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing header files
#-------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
BUILD		:=	build
SOURCES		:=	source \
				fatfs/source \
				$(LIBNTFS)/source
INCLUDES	:=	source \
				fatfs/source \
				$(LIBNTFS)/source \
				$(LIBNTFS)/include \
				include
#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
CFLAGS	:=	-Wall -Werror \
			-ffunction-sections -fdata-sections \
			$(MACHDEP) \
			$(BUILD_CFLAGS)

CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__ -DHAVE_CONFIG_H

ASFLAGS	:=	$(MACHDEP)

LDFLAGS	=	$(ARCH) -Wl,--gc-sections


LIBS	:=

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(WUT_ROOT) $(WUT_ROOT)/usr

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

#---------------------------------------------------------------------------------
# Explicit file list, NOT the usual $(wildcard $(dir)/*.c) - source/ also
# contains dvm_libogc2.c, dvm_libgba.c, dvm_calico.c, nitrofs.c,
# ext2_wrappers.c and ext4_driver.c (another platform's backend, and
# ext2/3/4 support - neither applies here), and fatfs/source/ also
# contains diskio.c and ffsystem.c (reference stubs that fat_driver.c /
# fat_wrappers.c replace with real Wii U-backed implementations instead
# of using - see fat_driver.c's disk_read()/get_fattime()/etc). This list
# mirrors upstream CMakeLists.txt's own per-target target_sources() calls
# for the platform-agnostic dvm core + the fat/exfat driver + FatFs itself,
# minus a GC/Wii-style platform backend file, which Wii U needs none of -
# dvmInit()/dvmDeinit()/_dvmIsAlignedAccess() are optional conveniences
# this build doesn't use.
#---------------------------------------------------------------------------------
CFILES		:=	dvm_disc.c \
				dvm_cache.c \
				dvm_volume.c \
				dvm_prober.c \
				fat_driver.c \
				fat_wrappers.c \
				ff.c \
				ffunicode.c \
				ntfs_driver.c \
				$(notdir $(wildcard $(LIBNTFS)/source/*.c))

#---------------------------------------------------------------------------------

export LD	:=	$(CC)

export OFILES_SRC	:=	$(CFILES:.c=.o)
export OFILES 	:=	$(OFILES_SRC)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I. 

.PHONY: all dist-bin dist-src dist install clean

#---------------------------------------------------------------------------------
all: lib/libdvm.a

dist-bin: all
	@tar --exclude=*~ -cjf libdvm-$(VERSION).tar.bz2 include lib

dist-src:
	@tar --exclude=*~ -cjf libdvm-src-$(VERSION).tar.bz2 include source fatfs/source Makefile

dist: dist-src dist-bin

install: dist-bin
	mkdir -p $(DESTDIR)$(DEVKITPRO)/wut/usr
	bzip2 -cd libdvm-$(VERSION).tar.bz2 | tar -xf - -C $(DESTDIR)$(DEVKITPRO)/wut/usr

lib:
	@[ -d $@ ] || mkdir -p $@

release:
	@[ -d $@ ] || mkdir -p $@

lib/libdvm.a :$(SOURCES) $(INCLUDES) | lib release
	@$(MAKE) BUILD=release OUTPUT=$(CURDIR)/$@ \
	BUILD_CFLAGS="-DNDEBUG=1 -O2 -s" \
	DEPSDIR=$(CURDIR)/release \
	--no-print-directory -C release \
	-f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -rf release lib

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT)	:	$(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------

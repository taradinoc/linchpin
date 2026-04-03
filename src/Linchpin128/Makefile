CC ?= gcc
CPPFLAGS ?=
CFLAGS ?= -Wall -Wextra -std=c99 -pedantic -O2
LDFLAGS ?=
LDLIBS ?=
INCLUDES = -Iinclude

MOS_C128_CLANG ?= $(HOME)/llvm-mos/bin/mos-c128-clang
MOS_CPPFLAGS ?=
MOS_CFLAGS ?= -Oz -Wall -Wextra -std=c99 -DLP128_ENABLE_PERF_COUNTERS=1
MOS_LDFLAGS ?= -T link_c128.ld
MOS_LDLIBS ?=

SRCDIR = src
OBJDIR = obj
BINDIR = bin

POSIX_SOURCES = $(SRCDIR)/lp128_bundle.c \
                $(SRCDIR)/lp128_vm.c \
                $(SRCDIR)/lp128_run.c \
                $(SRCDIR)/lp128_host_posix.c \
                $(SRCDIR)/main.c

C128_SOURCES = $(SRCDIR)/lp128_kernal.c \
               $(SRCDIR)/lp128_bundle.c \
               $(SRCDIR)/lp128_vm.c \
               $(SRCDIR)/lp128_run.c \
               $(SRCDIR)/lp128_reu.c \
               $(SRCDIR)/lp128_host_c128.c \
               $(SRCDIR)/main.c

SOURCES ?= $(POSIX_SOURCES)
OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
TARGET ?= $(BINDIR)/linchpin128
C128_TARGET ?= $(BINDIR)/CORNER128.PRG

# ── Disk image ──────────────────────────────────────────────────────────
# c1541 is the VICE disk-image tool.  On WSL it's a Windows executable, so
# all paths must be converted with wslpath -w before being passed to it.
C1541        ?= /mnt/d/vice/bin/c1541.exe
DISK_NAME    ?= corner128
DISK_FORMAT  ?= d71
DISK_TARGET  ?= $(BINDIR)/$(DISK_NAME).$(DISK_FORMAT)
# Optional: set BUNDLE=path/to/file.csb to also write a bundle onto the disk.
# c1541 interprets destination names as host text, and lowercase host text maps
# to the plain PETSCII bytes the KERNAL open/load path expects. Keep the disk
# image spelling lowercase even though the runtime requests uppercase bytes.
BUNDLE       ?=
BUNDLE_NAME  ?= corner.csb
# c1541 writes host files as PRG entries. Leave them as PRG; the runtime
# opens with ",P,R" to match.
#BUNDLE_DIR_TRACK  ?= 18
#BUNDLE_DIR_SECTOR ?= 1
#BUNDLE_DIR_OFFSET ?= 66
#BUNDLE_SEQ_TYPE   ?= 81
LAUNCHER_PRG  ?= $(BINDIR)/cornerboot.prg
LAUNCHER_NAME ?= cornerboot
PROGRAM_NAME  ?= corner128
# Current ML image loads at $1200; the custom loader calls the runtime entry
# points directly rather than using llvm-mos startup.
ML_ENTRY_ADDR ?= 4620

.PHONY: all posix c128 c128-d64 c128-d71 c128-d81 d64 d71 d81 disk clean .FORCE

# .FORCE has no recipe; any target that lists it as a prerequisite is always
# considered out of date (idiomatic GNU make pattern for forced rebuilds).

all: $(TARGET)

posix: all

c128:
	$(MAKE) clean
	$(MAKE) CC=$(MOS_C128_CLANG) CPPFLAGS="$(CPPFLAGS) $(MOS_CPPFLAGS)" CFLAGS="$(MOS_CFLAGS)" LDFLAGS="$(MOS_LDFLAGS)" LDLIBS="$(MOS_LDLIBS)" TARGET=$(C128_TARGET) SOURCES="$(C128_SOURCES)" all

# Build the C128 PRG then create the disk image in one step.
c128-d64:
	$(MAKE) c128
	$(MAKE) disk BUNDLE="$(BUNDLE)" DISK_FORMAT=d64

c128-d71:
	$(MAKE) c128
	$(MAKE) disk BUNDLE="$(BUNDLE)" DISK_FORMAT=d71

c128-d81:
	$(MAKE) c128
	$(MAKE) disk BUNDLE="$(BUNDLE)" DISK_FORMAT=d81

# Create a disk image from the already-built C128 PRG (and an optional BUNDLE).
# Requires bin/CORNER128.PRG to already exist (run 'make c128' first, or use
# 'make c128-d64' / 'make c128-d71' / 'make c128-d81' to do both in one go).
d64:
	$(MAKE) disk BUNDLE="$(BUNDLE)" DISK_FORMAT=d64

d71:
	$(MAKE) disk BUNDLE="$(BUNDLE)" DISK_FORMAT=d71

d81:
	$(MAKE) disk BUNDLE="$(BUNDLE)" DISK_FORMAT=d81

disk: $(DISK_TARGET)

# Auto-extract symbol addresses from the ELF so the launcher never goes stale.
ELF := $(C128_TARGET).elf

$(LAUNCHER_PRG): $(ELF) | $(BINDIR)
	$(eval _bss_start   := $(shell nm $< | awk '/ __bss_start$$/{print "0x"$$1}'))
	$(eval _bss_size    := $(shell nm $< | awk '/ __bss_size$$/{print "0x"$$1}'))
	$(eval _zp_bss_start := $(shell nm $< | awk '/ __zp_bss_start$$/{print "0x"$$1}'))
	$(eval _zp_bss_size := $(shell nm $< | awk '/ __zp_bss_size$$/{print "0x"$$1}'))
	$(eval _zp_data_start      := $(shell nm $< | awk '/ __zp_data_start$$/{print "0x"$$1}'))
	$(eval _zp_data_load_start := $(shell nm $< | awk '/ __zp_data_load_start$$/{print "0x"$$1}'))
	$(eval _zp_data_size       := $(shell nm $< | awk '/ __zp_data_size$$/{print "0x"$$1}'))
	$(eval _main        := $(shell nm $< | awk '/ main$$/{print "0x"$$1}'))
	$(eval _exit        := $(shell nm $< | awk '/ exit$$/{print "0x"$$1}'))
	$(eval _stack       := $(shell nm $< | awk '/ __stack$$/{print "0x"$$1}'))
	python3 make_launcher.py --output $(LAUNCHER_PRG) --program-name $(PROGRAM_NAME) --bss-start $(_bss_start) --bss-size $(_bss_size) --zp-bss-start $(_zp_bss_start) --zp-bss-size $(_zp_bss_size) --zp-data-start $(_zp_data_start) --zp-data-load-start $(_zp_data_load_start) --zp-data-size $(_zp_data_size) --main $(_main) --exit $(_exit) --stack $(_stack)

$(DISK_TARGET): $(LAUNCHER_PRG) $(C128_TARGET) $(BUNDLE) .FORCE | $(BINDIR)
	$(C1541) \
		-format "$(DISK_NAME),01" $(DISK_FORMAT) "$$(wslpath -w $(abspath $@))" \
		-write "$$(wslpath -w $(abspath $(LAUNCHER_PRG)))" $(LAUNCHER_NAME) \
		-write "$$(wslpath -w $(abspath $(C128_TARGET)))" $(PROGRAM_NAME) \
		$(if $(BUNDLE),-write "$$(wslpath -w $(abspath $(BUNDLE)))" "$(BUNDLE_NAME)")

$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BINDIR):
	mkdir -p $(BINDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(BINDIR)
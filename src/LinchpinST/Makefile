CC       ?= gcc
CPPFLAGS ?=
CFLAGS   ?= -Wall -Wextra -std=c99 -pedantic -O2
LD_CFLAGS ?= $(CFLAGS)
LDFLAGS  ?=
LDLIBS   ?= -lm
INCLUDES  = -Iinclude

ATARI_CC      ?= m68k-atari-mintelf-gcc
ATARI_CPPFLAGS ?= -DLPST_TARGET_ATARI=1
ATARI_CFLAGS  ?= -Wall -Wextra -std=c99 -pedantic -O2 -m68000 -fomit-frame-pointer
ATARI_LDLIBS ?=
ATARI_TOS_CC  ?= m68k-atari-mint-gcc
ATARI_TOS_CPPFLAGS ?= -DLPST_TARGET_ATARI=1
ATARI_TOS_CFLAGS ?= -Wall -Wextra -std=c99 -pedantic -O2 -m68000 -fomit-frame-pointer
ATARI_TOS_LDLIBS ?=
ATARI_TOS_PRG ?= $(BINDIR)/LINCHST_TOS.PRG
ATARI_TOS_TTP ?= $(BINDIR)/LINCHST.TTP
ATARI_DISK_BASENAME ?= CORNER
ATARI_DISK_LABEL ?= CORNERST
ATARI_STAGE_DIR ?= $(BINDIR)/atari-stage
ATARI_STAGE_ROOT ?= $(ATARI_STAGE_DIR)/$(ATARI_DISK_LABEL)
ATARI_STAGE_AUTO_DIR ?= $(ATARI_STAGE_ROOT)/AUTO
ATARI_STAGE_SAMPLE_DIR ?= $(ATARI_STAGE_ROOT)/SAMPLE
ATARI_DISK_IMAGE ?= $(BINDIR)/$(ATARI_DISK_BASENAME).st
ATARI_ZORK_DISK_BASENAME ?= ZORK
ATARI_ZORK_DISK_LABEL ?= ZORKST
ATARI_ZORK_STAGE_DIR ?= $(BINDIR)/atari-zork-stage
ATARI_ZORK_STAGE_ROOT ?= $(ATARI_ZORK_STAGE_DIR)/$(ATARI_ZORK_DISK_LABEL)
ATARI_ZORK_DISK_IMAGE ?= $(BINDIR)/$(ATARI_ZORK_DISK_BASENAME).st
ATARI_DISK_TRACKS ?= 80
ATARI_DISK_HEADS ?= 2
ATARI_DISK_SECTORS ?= 10
ATARI_MKFS_FAT ?= mkfs.fat
ATARI_MCOPY ?= mcopy
ATARI_TOUCH ?= touch
ATARI_FILE_TIMESTAMP ?= 198604011200.00
ATARI_MME ?= ../../Cornerstone/CORNER.MME
ATARI_OBJ ?= ../../Cornerstone/CORNER.OBJ
ATARI_ZORK1_MME ?= ../../../zilf/sample/zork1/zork1.mme
ATARI_ZORK1_OBJ ?= ../../../zilf/sample/zork1/zork1.obj
ATARI_SAMPLE_DIR ?= ../../Sample
ATARI_SYSTEM_TEMPLATE ?= SYBW.NEW
ATARI_SYSTEM_FILES = MS000255.CNR FS100000.CNR FS100099.CNR MS000000.NEW FF000000.NEW
ATARI_CNR_ALIAS_FILES = MS000000 FF000000 SY000000
ATARI_SAMPLE_FILES = $(wildcard $(ATARI_SAMPLE_DIR)/*.DBF)
AMIGA_CC      ?= m68k-amigaos-gcc
AMIGA_CPPFLAGS ?= -DLPST_TARGET_AMIGA=1
AMIGA_CFLAGS  ?= -Wall -Wextra -std=c99 -pedantic -O2 -m68000 -noixemul
AMIGA_LINK_CFLAGS ?= -Wall -Wextra -std=c99 -pedantic -O2 -m68000
AMIGA_LDFLAGS ?= -mcrt=nix13
AMIGA_LDLIBS  ?= -lm
AMIGA_TARGET  ?= $(BINDIR)/CORNER
AMIGA_FPU_CFLAGS ?= -Wall -Wextra -std=c99 -pedantic -O2 -m68040 -mhard-float -noixemul
AMIGA_FPU_LINK_CFLAGS ?= -Wall -Wextra -std=c99 -pedantic -O2 -m68040 -mhard-float
AMIGA_FPU_LDFLAGS ?= -mcrt=nix13
AMIGA_FPU_LDLIBS ?=
AMIGA_FPU_TARGET ?= $(BINDIR)/LINCHFPU
AMIGA_XDFTOOL ?= $(HOME)/venv/amitools/bin/xdftool
AMIGA_DISK_VOLUME ?= CORNER
AMIGA_STAGE_DIR ?= $(BINDIR)/amiga-stage
AMIGA_STAGE_ROOT ?= $(AMIGA_STAGE_DIR)/$(AMIGA_DISK_VOLUME)
AMIGA_DISK_IMAGE ?= $(BINDIR)/CORNER.adf
AMIGA_ADF_TYPE ?= adf
AMIGA_ADF_FS ?= ofs
AMIGA_ADF_BOOT ?= boot1x
AMIGA_HDF_IMAGE ?= $(BINDIR)/CORNER.hdf
AMIGA_FPU_DISK_VOLUME ?= LINCHPINFPU
AMIGA_FPU_STAGE_DIR ?= $(BINDIR)/amiga-fpu-stage
AMIGA_FPU_STAGE_ROOT ?= $(AMIGA_FPU_STAGE_DIR)/$(AMIGA_FPU_DISK_VOLUME)
AMIGA_FPU_DISK_IMAGE ?= $(BINDIR)/LINCHFPU.adf
AMIGA_FPU_HDF_IMAGE ?= $(BINDIR)/LINCHFPU.hdf
AMIGA_ZORK_FPU_DISK_VOLUME ?= ZORKFPU
AMIGA_ZORK_FPU_STAGE_DIR ?= $(BINDIR)/amiga-zork-fpu-stage
AMIGA_ZORK_FPU_STAGE_ROOT ?= $(AMIGA_ZORK_FPU_STAGE_DIR)/$(AMIGA_ZORK_FPU_DISK_VOLUME)
AMIGA_ZORK_FPU_HDF_IMAGE ?= $(BINDIR)/ZORKFPU.hdf
AMIGA_HDF_SIZE ?= 10M
AMIGA_MME ?= ../../Cornerstone/CORNER.MME
AMIGA_OBJ ?= ../../Cornerstone/CORNER.OBJ
AMIGA_MATHIEEE_DOUBBAS ?= lib/mathieeedoubbas.library
AMIGA_ZORK1_MME ?= ../../../zilf/sample/zork1/zork1.mme
AMIGA_ZORK1_OBJ ?= ../../../zilf/sample/zork1/zork1.obj
AMIGA_SAMPLE_DIR ?= ../../Sample
AMIGA_APP_NAME := $(notdir $(AMIGA_TARGET))
AMIGA_FPU_APP_NAME := $(notdir $(AMIGA_FPU_TARGET))
AMIGA_ZORK1_MME_NAME := $(notdir $(AMIGA_ZORK1_MME))
AMIGA_ZORK1_OBJ_NAME := $(notdir $(AMIGA_ZORK1_OBJ))

SRCDIR = src
OBJDIR = obj
BINDIR = bin

SOURCES = $(SRCDIR)/lpst_vm.c \
          $(SRCDIR)/lpst_image.c \
          $(SRCDIR)/lpst_host.c \
          $(SRCDIR)/lpst_host_console.c \
          $(SRCDIR)/lpst_exec.c \
		  $(SRCDIR)/lpst_run_core.c \
		  $(SRCDIR)/lpst_run_runtime.c \
		  $(SRCDIR)/lpst_run_dispatch_ext37.c \
		  $(SRCDIR)/lpst_run_dispatch_real.c \
		  $(SRCDIR)/lpst_run_dispatch_bits.c \
		  $(SRCDIR)/lpst_run_io.c \
		  $(SRCDIR)/lpst_run_dispatch.c \
          $(SRCDIR)/lpst_run.c \
          $(SRCDIR)/main.c

OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
TARGET  ?= $(BINDIR)/linchpin_st

.PHONY: all posix atari atari-tos atari-stage atari-disk atari-zork-stage atari-zork-disk zork-disk amiga amiga-stage amiga-adf amiga-hdf amiga-fpu amiga-fpu-stage amiga-fpu-adf amiga-fpu-hdf amiga-zork-fpu-stage amiga-zork-fpu-hdf clean

all: $(TARGET)

posix: all

atari:
	$(MAKE) clean
	$(MAKE) CC=$(ATARI_CC) CPPFLAGS="$(CPPFLAGS) $(ATARI_CPPFLAGS)" CFLAGS="$(ATARI_CFLAGS)" LDLIBS="$(ATARI_LDLIBS)" TARGET=$(BINDIR)/LINCHST.PRG all

atari-tos:
	$(MAKE) clean
	$(MAKE) CC=$(ATARI_TOS_CC) CPPFLAGS="$(CPPFLAGS) $(ATARI_TOS_CPPFLAGS)" CFLAGS="$(ATARI_TOS_CFLAGS)" LDLIBS="$(ATARI_TOS_LDLIBS)" TARGET=$(ATARI_TOS_PRG) all
	cp $(ATARI_TOS_PRG) $(ATARI_TOS_TTP)

atari-stage: atari-tos
	rm -rf $(ATARI_STAGE_DIR)
	mkdir -p $(ATARI_STAGE_AUTO_DIR)
	mkdir -p $(ATARI_STAGE_SAMPLE_DIR)
	cp $(ATARI_TOS_PRG) $(ATARI_STAGE_AUTO_DIR)/$(ATARI_DISK_BASENAME).PRG
	cp $(ATARI_MME) $(ATARI_STAGE_AUTO_DIR)/$(ATARI_DISK_BASENAME).MME
	cp $(ATARI_OBJ) $(ATARI_STAGE_AUTO_DIR)/$(ATARI_DISK_BASENAME).OBJ
	for file in $(ATARI_SYSTEM_FILES); do cp $(ATARI_SAMPLE_DIR)/$$file $(ATARI_STAGE_ROOT)/$$file; done
	cp $(ATARI_SAMPLE_DIR)/$(ATARI_SYSTEM_TEMPLATE) $(ATARI_STAGE_ROOT)/SY000000.NEW
	for file in $(ATARI_CNR_ALIAS_FILES); do cp $(ATARI_STAGE_ROOT)/$$file.NEW $(ATARI_STAGE_ROOT)/$$file.CNR; done
	cp $(ATARI_SAMPLE_FILES) $(ATARI_STAGE_SAMPLE_DIR)/
	find $(ATARI_STAGE_ROOT) -exec $(ATARI_TOUCH) -t $(ATARI_FILE_TIMESTAMP) {} +

atari-disk: atari-stage
	command -v $(ATARI_MKFS_FAT) >/dev/null 2>&1 || { echo "$(ATARI_MKFS_FAT) not found. Install dosfstools or set ATARI_MKFS_FAT=<path>."; exit 1; }
	command -v $(ATARI_MCOPY) >/dev/null 2>&1 || { echo "$(ATARI_MCOPY) not found. Install mtools or set ATARI_MCOPY=<path>."; exit 1; }
	rm -f $(ATARI_DISK_IMAGE)
	$(ATARI_MKFS_FAT) -C -A -F 12 -g $(ATARI_DISK_HEADS)/$(ATARI_DISK_SECTORS) -s 1 -r 64 -n $(ATARI_DISK_LABEL) $(ATARI_DISK_IMAGE) $$(( $(ATARI_DISK_TRACKS) * $(ATARI_DISK_HEADS) * $(ATARI_DISK_SECTORS) / 2 ))
	$(ATARI_MCOPY) -m -s -i $(ATARI_DISK_IMAGE) -o $(ATARI_STAGE_ROOT)/* ::

atari-zork-stage:
	rm -rf $(ATARI_ZORK_STAGE_DIR)
	mkdir -p $(ATARI_ZORK_STAGE_ROOT)
	cp $(ATARI_ZORK1_MME) $(ATARI_ZORK_STAGE_ROOT)/CORNER.MME
	cp $(ATARI_ZORK1_OBJ) $(ATARI_ZORK_STAGE_ROOT)/CORNER.OBJ
	find $(ATARI_ZORK_STAGE_ROOT) -exec $(ATARI_TOUCH) -t $(ATARI_FILE_TIMESTAMP) {} +

zork-disk: atari-zork-disk

atari-zork-disk: atari-zork-stage
	command -v $(ATARI_MKFS_FAT) >/dev/null 2>&1 || { echo "$(ATARI_MKFS_FAT) not found. Install dosfstools or set ATARI_MKFS_FAT=<path>."; exit 1; }
	command -v $(ATARI_MCOPY) >/dev/null 2>&1 || { echo "$(ATARI_MCOPY) not found. Install mtools or set ATARI_MCOPY=<path>."; exit 1; }
	rm -f $(ATARI_ZORK_DISK_IMAGE)
	$(ATARI_MKFS_FAT) -C -A -F 12 -g $(ATARI_DISK_HEADS)/$(ATARI_DISK_SECTORS) -s 1 -r 64 -n $(ATARI_ZORK_DISK_LABEL) $(ATARI_ZORK_DISK_IMAGE) $$(( $(ATARI_DISK_TRACKS) * $(ATARI_DISK_HEADS) * $(ATARI_DISK_SECTORS) / 2 ))
	$(ATARI_MCOPY) -m -s -i $(ATARI_ZORK_DISK_IMAGE) -o $(ATARI_ZORK_STAGE_ROOT)/* ::

amiga:
	$(MAKE) clean
	$(MAKE) CC=$(AMIGA_CC) CPPFLAGS="$(CPPFLAGS) $(AMIGA_CPPFLAGS)" CFLAGS="$(AMIGA_CFLAGS)" LD_CFLAGS="$(AMIGA_LINK_CFLAGS)" LDFLAGS="$(LDFLAGS) $(AMIGA_LDFLAGS)" LDLIBS="$(AMIGA_LDLIBS)" TARGET=$(AMIGA_TARGET) all

amiga-fpu:
	$(MAKE) clean
	$(MAKE) CC=$(AMIGA_CC) CPPFLAGS="$(CPPFLAGS) $(AMIGA_CPPFLAGS)" CFLAGS="$(AMIGA_FPU_CFLAGS)" LD_CFLAGS="$(AMIGA_FPU_LINK_CFLAGS)" LDFLAGS="$(LDFLAGS) $(AMIGA_FPU_LDFLAGS)" LDLIBS="$(AMIGA_FPU_LDLIBS)" TARGET=$(AMIGA_FPU_TARGET) all

amiga-stage: amiga
	rm -rf $(AMIGA_STAGE_DIR)
	mkdir -p $(AMIGA_STAGE_ROOT)
	mkdir -p $(AMIGA_STAGE_ROOT)/S
	cp $(AMIGA_TARGET) $(AMIGA_STAGE_ROOT)/$(AMIGA_APP_NAME)
	if [ -f $(AMIGA_MME) ]; then cp $(AMIGA_MME) $(AMIGA_STAGE_ROOT)/$(AMIGA_APP_NAME).MME; fi
	if [ -f $(AMIGA_OBJ) ]; then cp $(AMIGA_OBJ) $(AMIGA_STAGE_ROOT)/$(AMIGA_APP_NAME).OBJ; fi
	#if [ -f $(AMIGA_ZORK1_MME) ]; then cp $(AMIGA_ZORK1_MME) $(AMIGA_STAGE_ROOT)/$(AMIGA_ZORK1_MME_NAME); fi
	#if [ -f $(AMIGA_ZORK1_OBJ) ]; then cp $(AMIGA_ZORK1_OBJ) $(AMIGA_STAGE_ROOT)/$(AMIGA_ZORK1_OBJ_NAME); fi
	if [ -d $(AMIGA_SAMPLE_DIR) ]; then for file in $(ATARI_SYSTEM_FILES); do cp $(AMIGA_SAMPLE_DIR)/$$file $(AMIGA_STAGE_ROOT)/$$file; done; cp $(AMIGA_SAMPLE_DIR)/$(ATARI_SYSTEM_TEMPLATE) $(AMIGA_STAGE_ROOT)/SY000000.NEW; for file in $(ATARI_CNR_ALIAS_FILES); do cp $(AMIGA_STAGE_ROOT)/$$file.NEW $(AMIGA_STAGE_ROOT)/$$file.CNR; done; mkdir -p $(AMIGA_STAGE_ROOT)/Sample; cp $(AMIGA_SAMPLE_DIR)/*.DBF $(AMIGA_STAGE_ROOT)/Sample/; fi
	if [ -n "$(AMIGA_MATHIEEE_DOUBBAS)" ]; then if [ ! -f "$(AMIGA_MATHIEEE_DOUBBAS)" ]; then echo "AMIGA_MATHIEEE_DOUBBAS file not found: $(AMIGA_MATHIEEE_DOUBBAS)"; exit 1; fi; mkdir -p $(AMIGA_STAGE_ROOT)/Libs; cp "$(AMIGA_MATHIEEE_DOUBBAS)" $(AMIGA_STAGE_ROOT)/Libs/mathieeedoubbas.library; fi
	printf 'CD SYS:\n%s --data Sample\n' $(AMIGA_APP_NAME) > $(AMIGA_STAGE_ROOT)/S/startup-sequence
	find $(AMIGA_STAGE_ROOT) -exec $(ATARI_TOUCH) -t $(ATARI_FILE_TIMESTAMP) {} +

amiga-adf: amiga-stage
	command -v $(AMIGA_XDFTOOL) >/dev/null 2>&1 || { echo "$(AMIGA_XDFTOOL) not found. Install amitools (for example: pip3 install amitools) or set AMIGA_XDFTOOL=<path>."; exit 1; }
	rm -f $(AMIGA_DISK_IMAGE)
	$(AMIGA_XDFTOOL) -f $(AMIGA_DISK_IMAGE) create type=$(AMIGA_ADF_TYPE) + format $(AMIGA_DISK_VOLUME) $(AMIGA_ADF_FS) + boot install $(AMIGA_ADF_BOOT)
	cd $(AMIGA_STAGE_ROOT) && find . -mindepth 1 -type d | LC_ALL=C sort | while read rel_dir; do $(AMIGA_XDFTOOL) "$(abspath $(AMIGA_DISK_IMAGE))" makedir "$${rel_dir#./}"; done
	cd $(AMIGA_STAGE_ROOT) && find . -mindepth 1 -type f | LC_ALL=C sort | while read rel_file; do amiga_path=$${rel_file#./}; amiga_dir=$$(dirname "$$amiga_path"); if [ "$$amiga_dir" = "." ]; then amiga_dir=/; fi; $(AMIGA_XDFTOOL) "$(abspath $(AMIGA_DISK_IMAGE))" write "$$amiga_path" "$$amiga_dir"; done

amiga-hdf: amiga-stage
	command -v $(AMIGA_XDFTOOL) >/dev/null 2>&1 || { echo "$(AMIGA_XDFTOOL) not found. Install amitools (for example: pip3 install amitools) or set AMIGA_XDFTOOL=<path>."; exit 1; }
	rm -f $(AMIGA_HDF_IMAGE)
	$(AMIGA_XDFTOOL) $(AMIGA_HDF_IMAGE) create size=$(AMIGA_HDF_SIZE) + format $(AMIGA_DISK_VOLUME) ffs + boot install boot1x + makedir S + write $(AMIGA_STAGE_ROOT)/$(AMIGA_APP_NAME) / + write $(AMIGA_STAGE_ROOT)/$(AMIGA_APP_NAME).MME / + write $(AMIGA_STAGE_ROOT)/$(AMIGA_APP_NAME).OBJ / + write $(AMIGA_STAGE_ROOT)/S/startup-sequence S + write $(AMIGA_STAGE_ROOT)/Sample / + protect S/startup-sequence +srwed
	for file in $(ATARI_SYSTEM_FILES) SY000000.NEW $(addsuffix .CNR,$(ATARI_CNR_ALIAS_FILES)); do $(AMIGA_XDFTOOL) $(AMIGA_HDF_IMAGE) write $(AMIGA_STAGE_ROOT)/$$file /; done
	if [ -f $(AMIGA_STAGE_ROOT)/Libs/mathieeedoubbas.library ]; then $(AMIGA_XDFTOOL) $(AMIGA_HDF_IMAGE) makedir Libs + write $(AMIGA_STAGE_ROOT)/Libs/mathieeedoubbas.library Libs; fi
	if [ -f $(AMIGA_STAGE_ROOT)/$(AMIGA_ZORK1_MME_NAME) ]; then $(AMIGA_XDFTOOL) $(AMIGA_HDF_IMAGE) write $(AMIGA_STAGE_ROOT)/$(AMIGA_ZORK1_MME_NAME) /; fi
	if [ -f $(AMIGA_STAGE_ROOT)/$(AMIGA_ZORK1_OBJ_NAME) ]; then $(AMIGA_XDFTOOL) $(AMIGA_HDF_IMAGE) write $(AMIGA_STAGE_ROOT)/$(AMIGA_ZORK1_OBJ_NAME) /; fi

amiga-fpu-stage: amiga-fpu
	rm -rf $(AMIGA_FPU_STAGE_DIR)
	mkdir -p $(AMIGA_FPU_STAGE_ROOT)
	mkdir -p $(AMIGA_FPU_STAGE_ROOT)/S
	cp $(AMIGA_FPU_TARGET) $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_FPU_APP_NAME)
	if [ -f $(AMIGA_MME) ]; then cp $(AMIGA_MME) $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_FPU_APP_NAME).MME; fi
	if [ -f $(AMIGA_OBJ) ]; then cp $(AMIGA_OBJ) $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_FPU_APP_NAME).OBJ; fi
	if [ -f $(AMIGA_ZORK1_MME) ]; then cp $(AMIGA_ZORK1_MME) $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_ZORK1_MME_NAME); fi
	if [ -f $(AMIGA_ZORK1_OBJ) ]; then cp $(AMIGA_ZORK1_OBJ) $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_ZORK1_OBJ_NAME); fi
	if [ -d $(AMIGA_SAMPLE_DIR) ]; then for file in $(ATARI_SYSTEM_FILES); do cp $(AMIGA_SAMPLE_DIR)/$$file $(AMIGA_FPU_STAGE_ROOT)/$$file; done; cp $(AMIGA_SAMPLE_DIR)/$(ATARI_SYSTEM_TEMPLATE) $(AMIGA_FPU_STAGE_ROOT)/SY000000.NEW; for file in $(ATARI_CNR_ALIAS_FILES); do cp $(AMIGA_FPU_STAGE_ROOT)/$$file.NEW $(AMIGA_FPU_STAGE_ROOT)/$$file.CNR; done; mkdir -p $(AMIGA_FPU_STAGE_ROOT)/Sample; cp $(AMIGA_SAMPLE_DIR)/*.DBF $(AMIGA_FPU_STAGE_ROOT)/Sample/; fi
	printf 'CD SYS:\n%s\n' $(AMIGA_FPU_APP_NAME) > $(AMIGA_FPU_STAGE_ROOT)/S/startup-sequence
	find $(AMIGA_FPU_STAGE_ROOT) -exec $(ATARI_TOUCH) -t $(ATARI_FILE_TIMESTAMP) {} +

amiga-fpu-adf: amiga-fpu-stage
	command -v $(AMIGA_XDFTOOL) >/dev/null 2>&1 || { echo "$(AMIGA_XDFTOOL) not found. Install amitools (for example: pip3 install amitools) or set AMIGA_XDFTOOL=<path>."; exit 1; }
	rm -f $(AMIGA_FPU_DISK_IMAGE)
	$(AMIGA_XDFTOOL) -f $(AMIGA_FPU_DISK_IMAGE) create type=$(AMIGA_ADF_TYPE) + format $(AMIGA_FPU_DISK_VOLUME) $(AMIGA_ADF_FS) + boot install $(AMIGA_ADF_BOOT)
	cd $(AMIGA_FPU_STAGE_ROOT) && find . -mindepth 1 -type d | LC_ALL=C sort | while read rel_dir; do $(AMIGA_XDFTOOL) "$(abspath $(AMIGA_FPU_DISK_IMAGE))" makedir "$${rel_dir#./}"; done
	cd $(AMIGA_FPU_STAGE_ROOT) && find . -mindepth 1 -type f | LC_ALL=C sort | while read rel_file; do amiga_path=$${rel_file#./}; amiga_dir=$$(dirname "$$amiga_path"); if [ "$$amiga_dir" = "." ]; then amiga_dir=/; fi; $(AMIGA_XDFTOOL) "$(abspath $(AMIGA_FPU_DISK_IMAGE))" write "$$amiga_path" "$$amiga_dir"; done

amiga-fpu-hdf: amiga-fpu-stage
	command -v $(AMIGA_XDFTOOL) >/dev/null 2>&1 || { echo "$(AMIGA_XDFTOOL) not found. Install amitools (for example: pip3 install amitools) or set AMIGA_XDFTOOL=<path>."; exit 1; }
	rm -f $(AMIGA_FPU_HDF_IMAGE)
	$(AMIGA_XDFTOOL) $(AMIGA_FPU_HDF_IMAGE) create size=$(AMIGA_HDF_SIZE) + format $(AMIGA_FPU_DISK_VOLUME) ffs + boot install boot1x + makedir S + write $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_FPU_APP_NAME) / + write $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_FPU_APP_NAME).MME / + write $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_FPU_APP_NAME).OBJ / + write $(AMIGA_FPU_STAGE_ROOT)/S/startup-sequence S + write $(AMIGA_FPU_STAGE_ROOT)/Sample / + protect S/startup-sequence +srwed
	for file in $(ATARI_SYSTEM_FILES) SY000000.NEW $(addsuffix .CNR,$(ATARI_CNR_ALIAS_FILES)); do $(AMIGA_XDFTOOL) $(AMIGA_FPU_HDF_IMAGE) write $(AMIGA_FPU_STAGE_ROOT)/$$file /; done
	if [ -f $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_ZORK1_MME_NAME) ]; then $(AMIGA_XDFTOOL) $(AMIGA_FPU_HDF_IMAGE) write $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_ZORK1_MME_NAME) /; fi
	if [ -f $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_ZORK1_OBJ_NAME) ]; then $(AMIGA_XDFTOOL) $(AMIGA_FPU_HDF_IMAGE) write $(AMIGA_FPU_STAGE_ROOT)/$(AMIGA_ZORK1_OBJ_NAME) /; fi

amiga-zork-fpu-stage: amiga-fpu
	rm -rf $(AMIGA_ZORK_FPU_STAGE_DIR)
	mkdir -p $(AMIGA_ZORK_FPU_STAGE_ROOT)
	mkdir -p $(AMIGA_ZORK_FPU_STAGE_ROOT)/S
	cp $(AMIGA_FPU_TARGET) $(AMIGA_ZORK_FPU_STAGE_ROOT)/$(AMIGA_FPU_APP_NAME)
	cp $(AMIGA_ZORK1_MME) $(AMIGA_ZORK_FPU_STAGE_ROOT)/LINCHFPU.MME
	cp $(AMIGA_ZORK1_OBJ) $(AMIGA_ZORK_FPU_STAGE_ROOT)/LINCHFPU.OBJ
	if [ -d $(AMIGA_SAMPLE_DIR) ]; then cp -R $(AMIGA_SAMPLE_DIR) $(AMIGA_ZORK_FPU_STAGE_ROOT)/Sample; fi
	printf 'CD SYS:\n%s\n' $(AMIGA_FPU_APP_NAME) > $(AMIGA_ZORK_FPU_STAGE_ROOT)/S/startup-sequence

amiga-zork-fpu-hdf: amiga-zork-fpu-stage
	command -v $(AMIGA_XDFTOOL) >/dev/null 2>&1 || { echo "$(AMIGA_XDFTOOL) not found. Install amitools (for example: pip3 install amitools) or set AMIGA_XDFTOOL=<path>."; exit 1; }
	rm -f $(AMIGA_ZORK_FPU_HDF_IMAGE)
	$(AMIGA_XDFTOOL) $(AMIGA_ZORK_FPU_HDF_IMAGE) create size=$(AMIGA_HDF_SIZE) + format $(AMIGA_ZORK_FPU_DISK_VOLUME) ffs + boot install boot1x + makedir S + write $(AMIGA_ZORK_FPU_STAGE_ROOT)/$(AMIGA_FPU_APP_NAME) / + write $(AMIGA_ZORK_FPU_STAGE_ROOT)/LINCHFPU.MME / + write $(AMIGA_ZORK_FPU_STAGE_ROOT)/LINCHFPU.OBJ / + write $(AMIGA_ZORK_FPU_STAGE_ROOT)/S/startup-sequence S + write $(AMIGA_ZORK_FPU_STAGE_ROOT)/Sample / + protect S/startup-sequence +srwed

$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) $(LD_CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BINDIR):
	mkdir -p $(BINDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

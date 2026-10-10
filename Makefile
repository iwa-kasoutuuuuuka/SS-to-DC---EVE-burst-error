# EVE burst error engine for Dreamcast (KallistiOS)
#
#   make                   build eve_dc.elf for disc 1 (debug build: serial log on)
#   make cdi DISC=2        build/eve_dc_disc2.cdi from cd_root/Disc2 (made by tools/convert.py)
#   make release DISC=2    the same without debug output (-DNDEBUG)
#   make clean
#
# Extra compile switches (see the list at the top of main.c):
#   make cdi DISC=1 EXTRA_CFLAGS="-DNO_INTRO"

TARGET = eve_dc.elf
SRC_OBJS = main.o src/renderer.o src/vm.o src/asset.o src/input.o src/video.o src/audio.o \
           src/terminal.o src/bgm.o src/save.o src/adx/libadx.o src/adx/snddrv.o
OBJS = $(SRC_OBJS) romdisk.o

# DreamSDK exports KOS_BASE with backslashes; make wants forward slashes.
KOS_BASE := $(subst \,/,$(KOS_BASE))
ifeq ($(KOS_BASE),)
KOS_BASE := C:/DreamSDK/opt/toolchains/dc/kos
endif
KOS_PORTS := $(subst \,/,$(KOS_PORTS))
ifeq ($(KOS_PORTS),)
KOS_PORTS := $(KOS_BASE)/../kos-ports
endif
include $(KOS_BASE)/Makefile.rules

# bin2o (romdisk) needs these even when the KOS environment is only partly set.
KOS_AS      = sh-elf-as
KOS_AFLAGS  = -little
KOS_LD      = sh-elf-ld
KOS_OBJCOPY = sh-elf-objcopy

# Which disc this build is for: the engine reads /cd/scenario/Disc<n> and
# /cd/audio/Disc<n>, and asks for another disc when the script requires it.
DISC   ?= 1
CDROOT ?= cd_root/Disc$(DISC)
EVE_FLAGS = -DEVE_DISC=$(DISC) -DMULTI_DISC=1 $(EXTRA_CFLAGS)

# libADX is built from src/adx (a patched copy); do not link -lADX.
KOS_CFLAGS += -Iinclude -Isrc/adx -I$(KOS_PORTS)/libmpeg/inst/include \
              -ffast-math -ffp-contract=fast -funroll-loops $(EVE_FLAGS)
LIBS = -lmpeg -lkallisti -lm
KOS_LOCAL_LIBS = -Wl,--start-group $(LIBS) -Wl,--end-group

SCRAMBLE = $(KOS_BASE)/utils/scramble/scramble
BIN2O    = $(KOS_BASE)/utils/bin2o/bin2o
MAKEIP   = $(KOS_BASE)/utils/makeip/makeip
MKISOFS ?= mkisofs
CDI4DC  ?= cdi4dc

# Objects are rebuilt when DISC or EXTRA_CFLAGS change.
FLAGS_STAMP = build/flags.txt
$(shell mkdir -p build; echo "$(EVE_FLAGS)" | cmp -s - $(FLAGS_STAMP) || echo "$(EVE_FLAGS)" > $(FLAGS_STAMP))

all: $(TARGET)

release:
	$(MAKE) EXTRA_CFLAGS="-DNDEBUG $(EXTRA_CFLAGS)" cdi

clean:
	-rm -f $(TARGET) $(OBJS) romdisk.img
	-rm -rf build

$(SRC_OBJS): %.o: %.c $(FLAGS_STAMP)
	kos-cc $(KOS_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	kos-cc $(KOS_CFLAGS) $(KOS_LDFLAGS) -o $(TARGET) $(KOS_START) \
		$(OBJS) $(OBJEXTRA) $(KOS_LOCAL_LIBS)

romdisk.img: $(wildcard romdisk/*)
	$(KOS_GENROMFS) -f romdisk.img -d romdisk -v

romdisk.o: romdisk.img
	KOS_AS="$(KOS_AS)" KOS_AFLAGS="$(KOS_AFLAGS)" KOS_LD="$(KOS_LD)" KOS_OBJCOPY="$(KOS_OBJCOPY)" \
		$(BIN2O) romdisk.img romdisk romdisk.o

# Boot sector, generated from ip.txt (nothing is taken from a commercial disc).
build/IP.BIN: ip.txt
	$(MAKEIP) -f ip.txt build/IP.BIN

cdi: $(TARGET) build/IP.BIN
	@test -d $(CDROOT) || { echo "$(CDROOT) not found: run tools/convert.py first (see README.md)"; exit 1; }
	sh-elf-objcopy -R .stack -O binary $(TARGET) build/1ST_READ.raw
	$(SCRAMBLE) build/1ST_READ.raw build/1ST_READ.BIN
	$(MKISOFS) -V "EVE_DC" -G build/IP.BIN -r -J -l -graft-points \
		-o build/eve_dc_disc$(DISC).iso $(CDROOT) 1ST_READ.BIN=build/1ST_READ.BIN
	$(CDI4DC) build/eve_dc_disc$(DISC).iso build/eve_dc_disc$(DISC).cdi -d
	@echo "--- build/eve_dc_disc$(DISC).cdi ---"

.PHONY: all release clean cdi

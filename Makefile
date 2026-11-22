# ani3ds (ani-cli for New 3DS) -> build/ani3ds.{3dsx,cia}
# `make linux-test` builds the scraper test for the host.
DEVKITPRO ?= /opt/devkitpro
PREFIX    := $(DEVKITPRO)/devkitARM/bin/arm-none-eabi-
CC        := $(PREFIX)gcc
TOOLS     := $(DEVKITPRO)/tools/bin
MAKEROM   := /root/3ds-rc/tools/Project_CTR/makerom/bin/makerom
BANNERTOOL:= /root/3ds-rc/player/fourthtube/resource/tools/bannertool
FFMPEG    := /root/3ds-rc/player/ffmpeg-3ds
CABUNDLE  := /root/3ds-rc/netsurf/upstream/netsurf/resources/ca-bundle
OUT       := build

ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS    := -O2 -g -ffunction-sections -fdata-sections $(ARCH) -D__3DS__ -DNDEBUG -D_GNU_SOURCE \
             -I$(DEVKITPRO)/libctru/include -I$(DEVKITPRO)/portlibs/3ds/include -Isrc -Ivp -I$(FFMPEG)/include \
             -Wall -Wno-unused-parameter -Wno-format-truncation
LDFLAGS   := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(OUT)/ani3ds.map \
             -L$(DEVKITPRO)/portlibs/3ds/lib -L$(DEVKITPRO)/libctru/lib
LIBS      := -L$(FFMPEG)/lib -lavformat -lavcodec -lswresample -lavutil -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

SRC       := src/main3ds.c src/net.c src/scrape.c src/subs.c src/overlay.c vp/vp.c vp/vp_net.c
OBJ       := $(patsubst %.c,$(OUT)/%.o,$(notdir $(SRC)))
vpath %.c src vp

all: $(OUT)/ani3ds.3dsx $(OUT)/ani3ds.cia

$(OUT)/%.o: %.c $(wildcard src/*.h) vp/vp.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ani3ds.elf: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OUT)/romfs/ca-bundle: $(CABUNDLE)
	@mkdir -p $(OUT)/romfs
	cp $< $@

$(OUT)/ani3ds.smdh: 3ds/icon.png
	$(TOOLS)/smdhtool --create "ani3ds" "ani-cli for the New 3DS" "ani3ds" $< $@

$(OUT)/ani3ds.bnr: 3ds/banner.png 3ds/silence.wav
	$(BANNERTOOL) makebanner -i 3ds/banner.png -a 3ds/silence.wav -o $@

$(OUT)/ani3ds.3dsx: $(OUT)/ani3ds.elf $(OUT)/ani3ds.smdh $(OUT)/romfs/ca-bundle
	$(TOOLS)/3dsxtool $< $@ --smdh=$(OUT)/ani3ds.smdh --romfs=$(OUT)/romfs

$(OUT)/ani3ds.cia: $(OUT)/ani3ds.elf $(OUT)/ani3ds.smdh $(OUT)/ani3ds.bnr $(OUT)/romfs/ca-bundle 3ds/ani3ds.rsf
	cd $(OUT) && $(MAKEROM) -f cia -elf ani3ds.elf -rsf ../3ds/ani3ds.rsf -icon ani3ds.smdh -banner ani3ds.bnr -o ani3ds.cia

linux-test:
	gcc -O1 -g -Wall -Wno-unused-parameter -Isrc -o $(OUT)/t test/test_linux.c src/net.c src/scrape.c src/subs.c -lcurl -lpthread

clean:
	rm -rf $(OUT)
.PHONY: all clean linux-test

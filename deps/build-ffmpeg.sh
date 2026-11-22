#!/bin/sh
# ffmpeg for the 3DS video player: HLS + MPEG-TS + MP4, AAC/MP3/H.264 (MVD does video on hw)
set -e
export PATH=/opt/devkitpro/devkitARM/bin:$PATH
SRC=/root/3ds-rc/player/ffmpeg-src
OUT=/root/3ds-rc/player/ffmpeg-3ds
cd $SRC
./configure --enable-cross-compile --cross-prefix=arm-none-eabi- --prefix=$OUT \
  --cpu=armv6k --arch=arm --target-os=none \
  --extra-cflags="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -O2 -ffunction-sections -fdata-sections" \
  --extra-ldflags="-specs=3dsx.specs -mfloat-abi=hard -mtp=soft -L/opt/devkitpro/libctru/lib" --extra-libs="-lctru -lm" \
  --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-postproc --disable-swscale \
  --disable-network --disable-pthreads --disable-w32threads --disable-os2threads --disable-asm --enable-inline-asm \
  --enable-vfp --enable-armv5te --enable-armv6 --disable-debug --disable-iconv --disable-zlib --disable-bzlib --disable-lzma \
  --enable-avformat --enable-avcodec --enable-swresample --enable-avfilter \
  --enable-demuxer=hls,mpegts,mov,aac,mp3,flv,mpegtsraw \
  --enable-parser=h264,aac,aac_latm,mpegaudio \
  --enable-decoder=h264,aac,aac_latm,mp3,mp3float,opus \
  --enable-protocol=file,crypto,data \
  --enable-filter=aresample,aformat,anull,volume,atempo
# hook our curl-backed http/https protocol in
grep -q pbnet libavformat/Makefile || sed -i 's/^OBJS-\$(CONFIG_FILE_PROTOCOL)  *+= file.o/&\nOBJS += pbnet.o/' libavformat/Makefile
grep -q pbnet libavformat/Makefile || echo 'OBJS += pbnet.o' >> libavformat/Makefile
grep -q ff_pbnet libavformat/protocol_list.c || sed -i 's/^static const URLProtocol \* const url_protocols\[\] = {/extern const URLProtocol ff_pbnet_http_protocol;\nextern const URLProtocol ff_pbnet_https_protocol;\n&\n    \&ff_pbnet_http_protocol,\n    \&ff_pbnet_https_protocol,/' libavformat/protocol_list.c
cat libavformat/protocol_list.c
make -j8
make install

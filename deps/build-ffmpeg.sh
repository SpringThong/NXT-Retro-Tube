#!/bin/sh
set -e
# FFmpeg 6.1.6 (LGPL v3) with Mbed TLS, built static for tg5040.
# Unpack ffmpeg-6.1.6 into deps/ffmpeg-n6.1.6 first.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/deps
TOOLCHAIN=${TOOLCHAIN:-$ROOT/sdk/compiler}
TC=$TOOLCHAIN/bin/aarch64-nextui-linux-gnu-
SY=${SYSROOT:-$TOOLCHAIN/aarch64-nextui-linux-gnu/libc}
mkdir -p $D/mbed/lib && cp $ROOT/third_party/lib/libmbedtls.a $D/mbed/lib/ && ln -sf libmbedtls.a $D/mbed/lib/libmbedx509.a && ln -sf libmbedtls.a $D/mbed/lib/libmbedcrypto.a
rm -rf $D/mbed/include && cp -r $ROOT/third_party/include $D/mbed/include
cd $D/ffmpeg-n6.1.6
./configure --prefix=$D/ffmpeg-out --enable-cross-compile --arch=aarch64 --cpu=cortex-a53 --target-os=linux \
  --cross-prefix=$TC --sysroot=$SY --pkg-config=false \
  --enable-static --disable-shared --enable-pic --enable-version3 --enable-pthreads --enable-neon \
  --disable-programs --disable-doc --disable-debug --disable-everything --disable-autodetect \
  --disable-avdevice --disable-avfilter --disable-postproc --enable-swscale \
  --enable-mbedtls --extra-cflags="-I$D/mbed/include -O2" --extra-ldflags="-L$D/mbed/lib" \
  --enable-protocol=file,pipe,http,https,tcp,tls,crypto,hls \
  --enable-demuxer=mov,matroska,webm_dash_manifest,hls,mpegts,aac,mp3,ogg,flv \
  --enable-decoder=h264,hevc,vp9,vp8,aac,aac_latm,opus,mp3,mp3float,vorbis,mjpeg \
  --enable-parser=h264,hevc,vp9,vp8,aac,aac_latm,opus,mpegaudio,vorbis \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,vp9_superframe_split \
  --enable-swresample
make -j2
make install

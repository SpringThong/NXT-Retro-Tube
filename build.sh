#!/bin/sh
# Build NXT Retro Tube (aarch64) for TrimUI Brick / Brick Pro / Smart Pro S and RK3326 handhelds.
#   ROM=nxredux|nextui|stock|stock_sps|muos|knulli|darkos ./build.sh        (default: all seven)
# stock_sps = official firmware of the TrimUI Smart Pro S (A523); the same tg5040 toolchain
# works there (its glibc 2.33 and SDL2 2.32 are newer than the ones we link against).
# Reproducible: the version screen shows the build date; pin it with SOURCE_DATE_EPOCH=<seconds>.
# Needs the tg5040 toolchain + sysroot (see README_BUILD.md) and FFmpeg built by deps/build-ffmpeg.sh.
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
TOOLCHAIN=${TOOLCHAIN:-$ROOT/sdk/compiler}
CC=${CC:-$TOOLCHAIN/bin/aarch64-nextui-linux-gnu-gcc}
STRIP=${STRIP:-$TOOLCHAIN/bin/aarch64-nextui-linux-gnu-strip}
SYS=${SYSROOT:-$TOOLCHAIN/aarch64-nextui-linux-gnu/libc}
FF=${FFMPEG:-$ROOT/deps/ffmpeg-out}
TP=$ROOT/third_party
[ -x "$CC" ] || { echo "toolchain not found: $CC"; exit 1; }
[ -f "$FF/lib/libavformat.a" ] || { echo "FFmpeg not built: run deps/build-ffmpeg.sh"; exit 1; }
build_one() {
    ROM=$1; OUT=$ROOT/build/$ROM; mkdir -p "$OUT"
    ROMDEF=$(echo "$ROM" | tr a-z A-Z); DEVDEF=""
    case "$ROM" in stock_sps) ROMDEF=STOCK; DEVDEF="-DNXT_TUBE_DEV_TG5050";; esac
    # muOS / Knulli: evdev pad reader, PNG icons via stb_image (no SDL2_image), libstdc++ linked in
    EXTRA_SRC=""; IMGLIB="-lSDL2_image"; CXXLIB="-lstdc++"
    case "$ROM" in muos|knulli|darkos) EXTRA_SRC="$ROOT/src/input_evdev.c"; IMGLIB=""; CXXLIB="-Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -static-libgcc";; esac
    $CC -O2 -std=gnu11 -D_GNU_SOURCE -DNXT_TUBE_ROM_$ROMDEF $DEVDEF -Wall -Wno-unused-function -Wno-format-truncation \
      -ffunction-sections -fdata-sections -mcpu=cortex-a53 \
      -I$TP/include -I$FF/include -I$SYS/usr/include -I$SYS/usr/include/SDL2 \
      $ROOT/src/main.c $ROOT/src/draw.c $ROOT/src/player.c $ROOT/src/yt.c $ROOT/src/thumbs.c \
      $ROOT/src/store.c $ROOT/src/download.c $ROOT/src/session.c $ROOT/src/parson.c $ROOT/src/sonic.c \
      $ROOT/src/telex.c $ROOT/src/innertube.c $ROOT/src/system.c $ROOT/src/i18n.c $ROOT/src/settings.c $ROOT/src/audio_route.c $ROOT/src/kids.c $ROOT/src/subs.c $ROOT/src/ota.c $EXTRA_SRC \
      -Wl,--gc-sections -L$SYS/usr/lib -Wl,-rpath-link,$SYS/lib:$SYS/usr/lib \
      $FF/lib/libavformat.a $FF/lib/libavcodec.a $FF/lib/libswscale.a $FF/lib/libswresample.a $FF/lib/libavutil.a \
      $TP/lib/libmbedtls.a \
      $IMGLIB $TP/lib/text/libSDL2_ttf.a $TP/lib/text/libharfbuzz.a $TP/lib/text/libfreetype.a $TP/lib/text/libutf8proc.a $TP/lib/text/libfribidi.a \
      $CXXLIB -lSDL2 -lpthread -lm -lz -ldl -lrt \
      -o "$OUT/nxt-retro-tube.elf"
    $STRIP "$OUT/nxt-retro-tube.elf"
    ls -l "$OUT/nxt-retro-tube.elf"
}
if [ -n "$ROM" ]; then build_one "$ROM"; else for r in nxredux nextui stock stock_sps muos knulli darkos; do build_one $r; done; fi

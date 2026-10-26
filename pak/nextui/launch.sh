#!/bin/sh
# NXT Retro Tube 2.0 — by Nguyễn Xuân Thông — NextUI (TrimUI Brick / Brick Pro)
DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$DIR" || exit 1
PLATFORM="${PLATFORM:-tg5040}"
if [ "$PLATFORM" != tg5040 ]; then
    printf '%s\n' 'NXT Retro Tube: bản này dành cho TrimUI Brick / Brick Pro (tg5040).' > "$DIR/NXT_Tube_Log.txt"
    exit 1
fi
export PLATFORM DEVICE="${DEVICE:-brickpro}"
export LD_LIBRARY_PATH="$DIR/bin/$PLATFORM:${LD_LIBRARY_PATH:-}:/usr/lib"
export LIBASOUND_THREAD_SAFE=0
export NXT_TUBE_SOCKET="${NXT_TUBE_SOCKET:-/tmp/nxt-retro-tube-nextui-$(id -u).sock}"
DATA="${SHARED_USERDATA_PATH:-$DIR/data}/nxt-retro-tube"
mkdir -p "$DATA/cache" || exit 1
chmod +x "$DIR/bin/deno" "$DIR/bin/yt-dlp-runtime/yt-dlp" "$DIR/bin/$PLATFORM/nxt-retro-tube.elf" 2>/dev/null || :
# CPU speed is managed by the app (on-demand; full speed only for 60 fps / 1080p video)
BIN="$DIR/bin/$PLATFORM/nxt-retro-tube.elf"
LOG="$DATA/cache/nxt-retro-tube-run.txt"
# OTA: after an update the previous files wait in .ota/backup; if the new version
# fails to start twice in a row, put them back (the app clears .ota/pending once it runs)
ota_restore() {
    [ -d "$1" ] || return 0
    if [ -d "$1/yt-dlp-runtime" ]; then rm -rf "$2/yt-dlp-runtime"; mv "$1/yt-dlp-runtime" "$2/yt-dlp-runtime"; fi
    (cd "$1" && find . -type f) | while IFS= read -r F; do
        mkdir -p "$2/$(dirname "$F")" && mv -f "$1/$F" "$2/$F"
    done
}
if [ -f "$DIR/.ota/pending" ]; then
    T=$(cat "$DIR/.ota/tries" 2>/dev/null); T=$(( ${T:-0} + 1 )); echo "$T" > "$DIR/.ota/tries"
    if [ "$T" -ge 3 ]; then
        ota_restore "$DIR/.ota/backup/app" "$DIR"; ota_restore "$DIR/.ota/backup/bin" "$DIR/bin"
        rm -rf "$DIR/.ota/backup" "$DIR/.ota/pending" "$DIR/.ota/tries"
    fi
fi
"$BIN" > "$LOG" 2>&1 &
PID=$!
N=0
while [ "$N" -lt 300 ]; do
    if [ -f "$NXT_TUBE_SOCKET.state" ]; then
        read VIS SPID < "$NXT_TUBE_SOCKET.state"
        if [ "$VIS" = VISIBLE ] && kill -0 "$SPID" 2>/dev/null; then break; fi
    fi
    if ! kill -0 "$PID" 2>/dev/null && [ ! -f "$NXT_TUBE_SOCKET.state" ]; then exit 1; fi
    N=$((N+1)); sleep 0.1
done
# stay while the app is on screen; "Nghe nền" returns to NextUI with sound playing
while [ -f "$NXT_TUBE_SOCKET.state" ]; do
    read VIS SPID < "$NXT_TUBE_SOCKET.state"
    [ "$VIS" = VISIBLE ] || break
    kill -0 "$SPID" 2>/dev/null || break
    sleep 0.2
done
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$NXT_TUBE_SOCKET.state" ] && break
    [ -w "$g" ] && echo ondemand > "$g" 2>/dev/null
done
exit 0

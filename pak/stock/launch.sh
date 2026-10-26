#!/bin/sh
# NXT Retro Tube 2.0 — by Nguyễn Xuân Thông — official TrimUI firmware (Stock), TrimUI Brick / Brick Pro
DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)" || exit 1
cd "$DIR" || exit 1
STOCK_DATA="/mnt/SDCARD/.userdata/nxt-tube-stock"
export DEVICE="${DEVICE:-brickpro}" PLATFORM=tg5040
export USERDATA_PATH="$STOCK_DATA/system" SHARED_USERDATA_PATH="$STOCK_DATA/shared"
export NXT_TUBE_RUNTIME_DIR="/tmp/nxt-retro-tube-stock-$(id -u)"
export NXT_TUBE_SOCKET="$NXT_TUBE_RUNTIME_DIR/session.sock"
mkdir -p "$USERDATA_PATH" "$SHARED_USERDATA_PATH/nxt-retro-tube/cache" "$NXT_TUBE_RUNTIME_DIR" || exit 1
export LD_LIBRARY_PATH="$DIR/bin/tg5040:/usr/trimui/lib:${LD_LIBRARY_PATH:-}:/usr/lib:/lib"
export SDL_VIDEODRIVER=mali SDL_AUDIODRIVER=alsa LIBASOUND_THREAD_SAFE=0
chmod +x "$DIR/bin/deno" "$DIR/bin/yt-dlp-runtime/yt-dlp" "$DIR/bin/tg5040/nxt-retro-tube.elf" 2>/dev/null || :
BIN="$DIR/bin/tg5040/nxt-retro-tube.elf"
LOG="$SHARED_USERDATA_PATH/nxt-retro-tube/cache/nxt-retro-tube-run.txt"
PID=""

# The app pauses the firmware keymon while it is on screen and leaves a marker
# for each one; resume them here if the app ever ends without doing it.
recover() {
    for M in "$NXT_TUBE_RUNTIME_DIR"/keymon-*; do
        [ -f "$M" ] || continue
        K=${M##*keymon-}
        case "$K" in ''|*[!0-9]*) rm -f "$M"; continue ;; esac
        [ "$(cat "/proc/$K/comm" 2>/dev/null)" = keymon ] && kill -CONT "$K" 2>/dev/null
        rm -f "$M"
    done
    for F in stay_awake stay_alive; do
        M="$NXT_TUBE_RUNTIME_DIR/owned-$F"
        [ "$M" -ef "/tmp/$F" ] && rm -f "/tmp/$F"
        rm -f "$M"
    done
}
finish() {
    if [ -f "$NXT_TUBE_SOCKET.state" ]; then
        read VIS SPID < "$NXT_TUBE_SOCKET.state"
        kill -0 "$SPID" 2>/dev/null && return    # still playing in the background
    fi
    recover
}
trap finish EXIT
trap 'exit 1' INT TERM

if [ ! -f "$NXT_TUBE_SOCKET.state" ]; then recover; fi
# CPU speed is managed by the app (on-demand; full speed only for 60 fps / 1080p video)
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
# back to the stock Apps menu once the app hides; playback stays in the app process
while [ -f "$NXT_TUBE_SOCKET.state" ]; do
    read VIS SPID < "$NXT_TUBE_SOCKET.state"
    [ "$VIS" = VISIBLE ] || break
    kill -0 "$SPID" 2>/dev/null || break
    sleep 0.2
done
exit 0

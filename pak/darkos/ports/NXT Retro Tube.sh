#!/bin/bash
# NXT Retro Tube 2.1 — by Nguyễn Xuân Thông — dArkOS (R36S / RG351MP-style RK3326 handhelds)
# Ports entry: this script sits in /roms/ports (or /roms2/ports), the app in ./nxt-retro-tube
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DIR="$HERE/nxt-retro-tube"
cd "$DIR" || exit 1
BIN="$DIR/bin/tg5040/nxt-retro-tube.elf"
DATA="$DIR/data"
mkdir -p "$DATA/cache" || exit 1
LOG="$DATA/cache/nxt-retro-tube-run.txt"

# clear the text console under the picture (like the other dArkOS ports)
[ -w /dev/tty1 ] && printf '\033c' > /dev/tty1 2>/dev/null

export NXT_TUBE_SOCKET="/tmp/nxt-retro-tube-darkos-$(id -u).sock"
unset SHARED_USERDATA_PATH
chmod +x "$DIR/bin/deno" "$DIR/bin/yt-dlp-runtime/yt-dlp" "$BIN" 2>/dev/null || :

# the app starts its own session: "Nghe nền" returns to EmulationStation while the sound keeps playing
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
while [ -f "$NXT_TUBE_SOCKET.state" ]; do
    read VIS SPID < "$NXT_TUBE_SOCKET.state"
    [ "$VIS" = VISIBLE ] || break
    kill -0 "$SPID" 2>/dev/null || break
    sleep 0.2
done
[ -w /dev/tty1 ] && printf '\033c' > /dev/tty1 2>/dev/null
exit 0

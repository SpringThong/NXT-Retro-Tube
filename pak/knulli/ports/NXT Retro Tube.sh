#!/bin/bash
# NXT Retro Tube 2.1.2 — by Nguyễn Xuân Thông — Knulli (TrimUI Brick / Brick Pro)
# Ports entry: this script sits in /userdata/roms/ports, the app in ./nxt-retro-tube
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DIR="$HERE/nxt-retro-tube"
cd "$DIR" || exit 1
BIN="$DIR/bin/tg5040/nxt-retro-tube.elf"
DATA="$DIR/data"
mkdir -p "$DATA/cache" || exit 1
LOG="$DATA/cache/nxt-retro-tube-run.txt"

BOARD=$(cat /boot/boot/knulli.board 2>/dev/null || cat /boot/boot/batocera.board 2>/dev/null)
case "$BOARD" in
    trimui-brick|trimui-brick-pro|"") ;;
    *) printf '%s\n' "NXT Retro Tube: this build is for the TrimUI Brick / Brick Pro ($BOARD)" > "$DIR/NXT_Tube_Log.txt" ;;
esac

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run}"
export NXT_TUBE_SOCKET="/tmp/nxt-retro-tube-knulli-$(id -u).sock"
unset SHARED_USERDATA_PATH
chmod +x "$DIR/bin/deno" "$DIR/bin/yt-dlp-runtime/yt-dlp" "$BIN" 2>/dev/null || :

# flags a crashed run may have left (they hold our pid)
for F in /var/run/power-button.wake /var/run/suspend.debounce /var/run/battery-saver/nxt-retro-tube.pause; do
    [ -s "$F" ] || continue
    P=$(head -n1 "$F" 2>/dev/null)
    case "$P" in ''|*[!0-9]*) continue ;; esac
    [ -d "/proc/$P" ] || rm -f "$F"
done

GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
# CPU speed is managed by the app (on-demand; full speed only for 60 fps / 1080p video)

# the app starts its own session: "Nghe nền" returns to the menu while the sound keeps playing
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
# app closed (not playing in the background): give the CPU back to the system policy
if [ ! -f "$NXT_TUBE_SOCKET.state" ] && [ -n "$GOV" ]; then
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ -w "$g" ] && echo "$GOV" > "$g" 2>/dev/null
    done
fi
exit 0

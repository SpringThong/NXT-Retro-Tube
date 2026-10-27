#!/bin/sh
# HELP: NXT Retro Tube - YouTube for TrimUI Brick / Brick Pro
# ICON: nxt_tube
# GRID: NXT Tube

# NXT Retro Tube 2.1.1 — by Nguyễn Xuân Thông — muOS (TrimUI Brick / Brick Pro)
DIR="${1:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}"
cd "$DIR" || exit 1
BIN="$DIR/bin/tg5040/nxt-retro-tube.elf"
DATA="$DIR/data"
mkdir -p "$DATA/cache" || exit 1
LOG="$DATA/cache/nxt-retro-tube-run.txt"

case "$(cat /opt/muos/device/config/board/name 2>/dev/null)" in
    tui-brick|tui-brick-pro|"") ;;
    *) printf '%s\n' "NXT Retro Tube: this build is for the TrimUI Brick / Brick Pro" > "$DIR/NXT_Tube_Log.txt" ;;
esac

# muOS helpers (frontend state, controller DB, governor); older releases lack SETUP_APP
[ -f /opt/muos/script/var/retro.sh ] && . /opt/muos/script/var/retro.sh
if command -v SETUP_APP >/dev/null 2>&1; then
    SETUP_APP "nxt-retro-tube.elf" ""
else
    echo app > /tmp/act_go 2>/dev/null
    command -v SETUP_SDL_ENVIRONMENT >/dev/null 2>&1 && SETUP_SDL_ENVIRONMENT
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run}" PIPEWIRE_RUNTIME_DIR="${PIPEWIRE_RUNTIME_DIR:-/run}"
export NXT_TUBE_SOCKET="/tmp/nxt-retro-tube-muos-$(id -u).sock"
unset SHARED_USERDATA_PATH
chmod +x "$DIR/bin/deno" "$DIR/bin/yt-dlp-runtime/yt-dlp" "$BIN" 2>/dev/null || :

# flags a crashed run may have left (they hold our pid)
for F in /run/muos/caffeine /run/muos/idle_game_inhibit; do
    [ -s "$F" ] || continue
    P=$(head -n1 "$F" 2>/dev/null)
    case "$P" in ''|*[!0-9]*) continue ;; esac
    [ -d "/proc/$P" ] || rm -f "$F"
done

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
exit 0

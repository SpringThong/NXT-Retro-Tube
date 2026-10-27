#!/bin/sh
# NXT Retro Tube 2.1.3 — by Nguyễn Xuân Thông — official TrimUI firmware (Stock), TrimUI Smart Pro S (TG5050)
DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)" || exit 1
cd "$DIR" || exit 1
STOCK_DATA="/mnt/SDCARD/.userdata/nxt-tube-stock"
export DEVICE="${DEVICE:-smartpros}" PLATFORM=tg5050
export USERDATA_PATH="$STOCK_DATA/system" SHARED_USERDATA_PATH="$STOCK_DATA/shared"
export NXT_TUBE_RUNTIME_DIR="/tmp/nxt-retro-tube-stock-$(id -u)"
export NXT_TUBE_SOCKET="$NXT_TUBE_RUNTIME_DIR/session.sock"
mkdir -p "$USERDATA_PATH" "$SHARED_USERDATA_PATH/nxt-retro-tube/cache" "$NXT_TUBE_RUNTIME_DIR" || exit 1
# Display stages (see sps_open_display in src/main.c): 0 = firmware SDL (/usr/trimui/lib) with GPU,
# 1 = system SDL (/usr/lib) with GPU, 2 = firmware SDL with GPU, else CPU drawing. The app exits
# with code 3 when a stage has no way to open the screen; the stage that works is remembered.
LD_TRIMUI="$DIR/bin/tg5040:/usr/trimui/lib:${LD_LIBRARY_PATH:-}:/usr/lib:/lib"
LD_SYSTEM="$DIR/bin/tg5040:/usr/lib:/lib:/usr/trimui/lib:${LD_LIBRARY_PATH:-}"
STAGE_FILE="$STOCK_DATA/display-stage"
STAGE=$(cat "$STAGE_FILE" 2>/dev/null); case "$STAGE" in 0|1|2) ;; *) STAGE=0 ;; esac
# the firmware keeps keymon (volume, power, Home); the video driver it sets is kept too
export SDL_AUDIODRIVER=alsa LIBASOUND_THREAD_SAFE=0
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
# --- the screen (DRM master) -------------------------------------------------------
# If the TrimUI menu keeps the panel while the app starts, the app exits with code 4.
# Then the program holding /dev/dri/card* is parked: its parent shell loop (if any) is
# paused so it does not respawn it, the holder is stopped, and everything is put back
# (menu restarted) when the app closes or goes to the background. Remembered for next time.
PARK_FILE="$STOCK_DATA/display-park"
PARKED=""
drm_holders() {
    for P in /proc/[0-9]*; do
        HP=${P#/proc/}
        [ "$HP" = "$$" ] && continue
        for F in "$P"/fd/*; do
            case "$(readlink "$F" 2>/dev/null)" in /dev/dri/card*) echo "$HP $(sed -n 's/^PPid:[[:space:]]*//p' "$P/status" 2>/dev/null) $(cat "$P/comm" 2>/dev/null)"; break ;; esac
        done
    done
}
park() {
    [ -n "$PARKED" ] && return 0
    drm_holders > "$NXT_TUBE_RUNTIME_DIR/park-list"
    while read HP PP HC; do
        case "$HC" in nxt-retro-tube*|keymon|trimui_inputd|trimui_osdd|init|"") continue ;; esac
        [ -d "/proc/$HP" ] || continue
        case " $PARKED " in *" $PP "*) kill -TERM "$HP" 2>/dev/null; continue ;; esac   # a child of a parked program
        PC=$(cat "/proc/$PP/comm" 2>/dev/null)
        { tr '\0' ' ' < "/proc/$HP/cmdline" > "$NXT_TUBE_RUNTIME_DIR/parked-$HP.cmd"
          readlink "/proc/$HP/cwd" > "$NXT_TUBE_RUNTIME_DIR/parked-$HP.cwd"
          tr '\0' '\n' < "/proc/$HP/environ" > "$NXT_TUBE_RUNTIME_DIR/parked-$HP.env"; } 2>/dev/null
        echo "$HC" > "$NXT_TUBE_RUNTIME_DIR/parked-$HP.comm"
        LOOP=""
        case "$PC" in sh|ash|bash|*.sh) if [ "$PP" != 1 ] && [ "$PP" != "$$" ] && kill -STOP "$PP" 2>/dev/null; then LOOP="$PP"; fi ;; esac
        echo "$LOOP" > "$NXT_TUBE_RUNTIME_DIR/parked-$HP.loop"
        echo "launch.sh: parking $HC ($HP), parent $PC ($PP)${LOOP:+ paused}" >> "$LOG"
        kill -TERM "$HP" 2>/dev/null
        I=0; while [ -d "/proc/$HP" ] && [ "$I" -lt 20 ]; do sleep 0.1; I=$((I+1)); done
        [ -d "/proc/$HP" ] && kill -KILL "$HP" 2>/dev/null
        PARKED="$PARKED $HP"
    done < "$NXT_TUBE_RUNTIME_DIR/park-list"
    [ -n "$PARKED" ]
}
unpark() {
    for HP in $PARKED; do
        LOOP=$(cat "$NXT_TUBE_RUNTIME_DIR/parked-$HP.loop" 2>/dev/null)
        HC=$(cat "$NXT_TUBE_RUNTIME_DIR/parked-$HP.comm" 2>/dev/null)
        if [ -n "$LOOP" ] && [ -d "/proc/$LOOP" ]; then
            kill -CONT "$LOOP" 2>/dev/null          # its loop starts the menu again
            echo "launch.sh: resumed $HC's parent ($LOOP)" >> "$LOG"
        else
            CMD=$(cat "$NXT_TUBE_RUNTIME_DIR/parked-$HP.cmd" 2>/dev/null); CWD=$(cat "$NXT_TUBE_RUNTIME_DIR/parked-$HP.cwd" 2>/dev/null)
            if [ -n "$CMD" ]; then
                ( cd "${CWD:-/}" 2>/dev/null
                  while IFS= read -r L; do case "$L" in *=*) export "$L" ;; esac; done < "$NXT_TUBE_RUNTIME_DIR/parked-$HP.env"
                  exec setsid $CMD > /dev/null 2>&1 ) &
                echo "launch.sh: restarted $HC: $CMD" >> "$LOG"
            fi
        fi
        rm -f "$NXT_TUBE_RUNTIME_DIR"/parked-"$HP".*
    done
    PARKED=""
}
finish() {
    unpark
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
start_app() {
    if [ "$STAGE" = 1 ]; then export LD_LIBRARY_PATH="$LD_SYSTEM"; else export LD_LIBRARY_PATH="$LD_TRIMUI"; fi
    export NXT_TUBE_DISPLAY_STAGE="$STAGE"
    echo "launch.sh: display stage $STAGE${PARKED:+ (screen parked)}" >> "$LOG"
    "$BIN" >> "$LOG" 2>&1 &          # SIGHUP stays ignored (trap below) if the menu that started us is parked
    PID=$!
}
trap '' HUP          # the menu that started us may be parked: keep running
: > "$LOG"
if [ "$(cat "$PARK_FILE" 2>/dev/null)" = 1 ]; then park; export NXT_TUBE_DRM_PARKED=1; fi
start_app
N=0
while [ "$N" -lt 300 ]; do
    if [ -f "$NXT_TUBE_SOCKET.state" ]; then
        read VIS SPID < "$NXT_TUBE_SOCKET.state"
        if [ "$VIS" = VISIBLE ] && kill -0 "$SPID" 2>/dev/null; then
            echo "$STAGE" > "$STAGE_FILE"
            if [ -n "$NXT_TUBE_DRM_PARKED" ]; then echo 1 > "$PARK_FILE"; else rm -f "$PARK_FILE"; fi
            break
        fi
    fi
    if ! kill -0 "$PID" 2>/dev/null && [ ! -f "$NXT_TUBE_SOCKET.state" ]; then
        wait "$PID"; RC=$?
        if [ "$RC" = 3 ] && [ "$STAGE" -lt 2 ]; then STAGE=$((STAGE + 1)); start_app; N=0; continue; fi
        # 4 = the screen is held: park its holder (if one can be found) and start once more either way
        if [ "$RC" = 4 ] && [ -z "$NXT_TUBE_DRM_PARKED" ]; then park; export NXT_TUBE_DRM_PARKED=1; start_app; N=0; continue; fi
        echo "launch.sh: app ended with code $RC" >> "$LOG"
        exit 1
    fi
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

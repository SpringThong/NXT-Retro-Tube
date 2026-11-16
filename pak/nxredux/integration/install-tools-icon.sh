#!/bin/sh
# NXT_COMMON_ICON_WRAPPER 2 - NXT Retro Tube: Tools-menu icons for the NXT apps on NX Redux.
# (The marker above tells NXT Icon Repair not to wrap this script: it already defers to it.)
# The six colour icons (Music, Tube, Movie, BookNest, BrickTok, NXT File) are installed by the
# NXT Icon Repair engine:
#   - NXT Icon Repair is on the card  -> that tool does the work (its chosen style is kept);
#   - otherwise the copy in icon-repair/ runs with the colour set;
#   - a launcher that engine does not know -> the older two-icon patcher (install-tools-icon-legacy.sh).
#   install-tools-icon.sh            install or refresh (also turns automatic repair back on)
#   install-tools-icon.sh --auto     the same, at every start of the app, unless the user removed the icons
#   install-tools-icon.sh --status   installed | ready | unsupported
#   install-tools-icon.sh --restore  put the previous launcher and icons back
DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)" || exit 1
SD="${SDCARD_PATH:-/mnt/SDCARD}"
EMB="$DIR/icon-repair"
LEGACY="$DIR/install-tools-icon-legacy.sh"
COMMON=""
for C in "$SD/Tools/NXT Icon Repair.pak/repair.sh" "$SD/Tools/tg5040/NXT Icon Repair.pak/repair.sh"; do [ -f "$C" ] && { COMMON="$C"; break; }; done
OFF="$EMB/state/disabled"
embedded() { chmod +x "$EMB/nxt-icon-engine" 2>/dev/null || :; NXT_ICON_EMBEDDED=1 sh "$EMB/repair.sh" "$@"; }
legacy() { [ -f "$LEGACY" ] && sh "$LEGACY" "$@"; }
case "${1:-}" in
    --status)
        if [ -n "$COMMON" ]; then RESULT=$(sh "$COMMON" --status 2>&1) || RESULT=""; else RESULT=$(embedded --status 2>&1) || RESULT=""; fi
        case "$RESULT" in
            INSTALLED:*) echo installed ;;
            READY:*|UPGRADE:*) echo ready ;;
            *) L=$(legacy --status 2>/dev/null | tail -n 1); case "$L" in installed|ready) echo "$L" ;; *) echo unsupported ;; esac ;;
        esac
        ;;
    --restore)
        mkdir -p "$EMB/state"; : > "$OFF"
        if [ -n "$COMMON" ]; then sh "$COMMON" --restore; RC=$?; else embedded --restore; RC=$?; fi
        [ "$RC" = 0 ] || legacy --restore
        ;;
    *)
        if [ "${1:-}" = --auto ]; then [ ! -f "$OFF" ] || { echo "Icon NXT: đã gỡ theo yêu cầu, không tự cài lại"; exit 0; }; else rm -f "$OFF"; fi
        if [ -n "$COMMON" ]; then sh "$COMMON" --boot; exit $?; fi
        mkdir -p "$EMB/state"
        [ -f "$EMB/state/style.txt" ] || printf '%s\n' color > "$EMB/state/style.txt"
        embedded --boot; RC=$?
        if [ "$RC" != 0 ]; then legacy; RC=$?; fi
        exit "$RC"
        ;;
esac

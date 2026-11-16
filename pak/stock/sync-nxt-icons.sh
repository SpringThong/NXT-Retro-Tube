#!/bin/sh
# NXT Retro Tube — one look for the NXT apps on the official TrimUI firmware (Stock).
# Every NXT app found in Apps/ (Music, Tube, Movie, BookNest, BrickTok, NXT File) gets the matching icon of
# the colour set: the same size as the firmware's own icons, with a thin dark edge so it reads on the blue
# background and on any theme. Only the picture files an app's config.json names are replaced; the first
# original is kept beside it as <name>.nxt-before.
#   sync-nxt-icons.sh            apply (runs at every start of NXT Retro Tube; a no-op when nothing changed)
#   sync-nxt-icons.sh --restore  put the saved originals back
DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)" || exit 1
SRC="$DIR/nxt-icons"
APPS="${NXT_APPS_DIR:-/mnt/SDCARD/Apps}"
[ -d "$SRC" ] && [ -d "$APPS" ] || exit 0
OFF="$DIR/nxt-icons.off"
if [ "${1:-}" = --restore ]; then : > "$OFF"; else [ ! -f "$OFF" ] || { echo "NXT icons: removed on request, left alone"; exit 0; }; fi

icon_for() {   # $1 = folder name -> the icon file of that app, or nothing
    N=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -cd '[:alnum:]')
    case "$N" in nxt*) ;; *) return 1 ;; esac
    case "$N" in
        *iconrepair*|*iconrestore*) return 1 ;;
        *tube*) echo menu_icon_nxt_tube.png ;;
        *movie*) echo menu_icon_nxt_retro_movie.png ;;
        *booknest*|*comic*) echo menu_icon_nxt_booknest.png ;;
        *bricktok*) echo menu_icon_nxt_bricktok.png ;;
        *file*) echo menu_icon_nxt_file.png ;;
        *music*|*soloist*) echo menu_icon_nxt.png ;;
        *) return 1 ;;
    esac
}
names_in_config() {   # the picture files config.json points at ("icon", "icontop")
    tr ',{}' '\n\n\n' < "$1" 2>/dev/null | sed -n 's/.*"icon\(top\)\{0,1\}"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\2/p'
}
for APP in "$APPS"/*; do
    [ -d "$APP" ] && [ ! -L "$APP" ] && [ -f "$APP/config.json" ] || continue
    ICON=$(icon_for "$(basename "$APP")") || continue
    names_in_config "$APP/config.json" | sort -u | while IFS= read -r F; do
        case "$F" in ''|*/*|*..*) continue ;; esac            # a plain file name inside the app's own folder
        case "$F" in *.png|*.PNG) ;; *) continue ;; esac
        DEST="$APP/$F"
        [ -f "$DEST" ] && [ ! -L "$DEST" ] || continue
        # an app that checks its own files against a checksum list is left untouched (it would refuse to start)
        if grep -qsF "$F" "$APP"/checksums.sha256 "$APP"/*.sha256 2>/dev/null; then echo "kept (checksum list): $DEST"; continue; fi
        SAVED="$DEST.nxt-before"
        if [ "${1:-}" = --restore ]; then
            [ -f "$SAVED" ] && cmp -s "$SRC/$ICON" "$DEST" && { cp -p "$SAVED" "$DEST.nxt-$$" && mv "$DEST.nxt-$$" "$DEST" && rm -f "$SAVED" && echo "restored: $DEST"; }
            continue
        fi
        cmp -s "$SRC/$ICON" "$DEST" && continue
        [ -f "$SAVED" ] || cp -p "$DEST" "$SAVED" || continue
        cp "$SRC/$ICON" "$DEST.nxt-$$" && mv "$DEST.nxt-$$" "$DEST" && echo "icon: $DEST"
    done
done
exit 0

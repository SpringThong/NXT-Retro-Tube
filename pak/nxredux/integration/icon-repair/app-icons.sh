#!/bin/sh
# Icon files only. Back up an app's existing artwork before installing the set.
# A BookNest launch republishes its local menu PNG; update that source as well.
apply_app_icon() {
    REL="$1"
    SOURCE_ICON="$ICON_SOURCE/$2"
    DEST_ICON="$SD/$REL"
    [ -f "$DEST_ICON" ] && [ ! -L "$DEST_ICON" ] || return 0
    [ ! -L "$(dirname "$DEST_ICON")" ] || return 0
    cmp -s "$SOURCE_ICON" "$DEST_ICON" && return 0
    SAVED_ICON="$STATE/app-icons/$REL"
    mkdir -p "$(dirname "$SAVED_ICON")"
    if [ ! -f "$SAVED_ICON" ]; then
        cp -p "$DEST_ICON" "$SAVED_ICON.new"
        mv "$SAVED_ICON.new" "$SAVED_ICON"
        hash "$SAVED_ICON" > "$SAVED_ICON.original.sha256"
    fi
    [ "$(hash "$SAVED_ICON")" = "$(cat "$SAVED_ICON.original.sha256")" ] || die "App icon backup mismatch: $REL"
    cp "$SOURCE_ICON" "$DEST_ICON.nxt-$$"
    mv "$DEST_ICON.nxt-$$" "$DEST_ICON"
    hash "$SOURCE_ICON" > "$SAVED_ICON.managed.sha256"
    log "ICON: $REL"
}

install_app_icons() {
    for APP in "$SD/Tools"/*.pak "$SD/Tools/tg5040"/*.pak; do
        [ -d "$APP" ] && [ ! -L "$APP" ] || continue
        APPNAME=$(basename "$APP" .pak | tr '[:upper:]' '[:lower:]' | tr -cd '[:alnum:]')
        case "$APPNAME" in
            nxtretromusic|nxtretromusicsoloist|nxtmusicsoloist) ICON=menu_icon_nxt.png ;;
            nxtretrotube|nxttube) ICON=menu_icon_nxt_tube.png ;;
            nxtretromovie|nxtmovie) ICON=menu_icon_nxt_retro_movie.png ;;
            nxtretrobooknest|nxtbooknest|nxtretrocomic) ICON=menu_icon_nxt_booknest.png ;;
            nxtretrobricktok|nxtbricktok|nxtretrobricktoklogin1) ICON=menu_icon_nxt_bricktok.png ;;
            nxtretrofile|nxtfile|nxtretrofiles|nxtfiles) ICON=menu_icon_nxt_file.png ;;
            *) continue ;;
        esac
        APPREL=${APP#"$SD/"}
        if [ "$ICON" = menu_icon_nxt_file.png ]; then
            # NXT File checks icon.png against its package checksum manifest
            # before starting. Its NX Redux menu uses the shared launcher PNG,
            # so keep the whole app package intact and repair only menu art.
            log "ICON: $APPREL uses shared launcher artwork; package kept intact"
            continue
        fi
        apply_app_icon "$APPREL/icon.png" "$ICON"
        if [ "$ICON" = menu_icon_nxt_booknest.png ] && [ ! -L "$APP/resources" ]; then
            # BookNest --install-icons republishes resources/icon.png on launch.
            apply_app_icon "$APPREL/resources/icon.png" "$ICON"
        fi
        # Only files already present in known icon directories are replaced.
        for ICON_DIR in '' assets integration; do
            [ ! -L "$APP/$ICON_DIR" ] || continue
            case "$ICON_DIR" in '') PREFIX="$APPREL" ;; *) PREFIX="$APPREL/$ICON_DIR" ;; esac
            apply_app_icon "$PREFIX/$ICON" "$ICON"
        done
        # Tube also carries Music's icon for its legacy shared installer.
        if [ "$ICON" = menu_icon_nxt_tube.png ] && [ ! -L "$APP/integration" ]; then
            apply_app_icon "$APPREL/integration/menu_icon_nxt.png" menu_icon_nxt.png
        fi
    done
}

restore_app_icons() {
    [ -d "$STATE/app-icons" ] || return 0
    find "$STATE/app-icons" -type f -name '*.original.sha256' | while IFS= read -r RECORD; do
        SAVED_ICON=${RECORD%.original.sha256}
        REL=${SAVED_ICON#"$STATE/app-icons/"}
        case "$REL" in Tools/*.pak/*|Tools/tg5040/*.pak/*) ;; *) continue ;; esac
        DEST_ICON="$SD/$REL"
        [ -f "$DEST_ICON" ] && [ ! -L "$DEST_ICON" ] || continue
        [ ! -L "$(dirname "$DEST_ICON")" ] || continue
        MANAGED=$(cat "$SAVED_ICON.managed.sha256" 2>/dev/null || :)
        if [ "$(hash "$DEST_ICON")" != "$MANAGED" ]; then
            log "ICON: kept later artwork changes: $REL"
            continue
        fi
        [ "$(hash "$SAVED_ICON")" = "$(cat "$RECORD")" ] || { log "ICON: backup checksum failed: $REL"; continue; }
        cp -p "$SAVED_ICON" "$DEST_ICON.nxt-$$"
        mv "$DEST_ICON.nxt-$$" "$DEST_ICON"
        log "ICON: restored $REL"
    done
}

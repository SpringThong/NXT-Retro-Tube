#!/bin/sh
# NXT Icon Repair 1.2 - NX Redux / TrimUI Brick Pro, six-app icon set
# Copy carried by NXT Retro Tube (colour set only). NXT_ICON_EMBEDDED=1 skips redirecting other apps' old installers.
set -eu
DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SD="${SDCARD_PATH:-/mnt/SDCARD}"
SYS="${SYSTEM_PATH:-$SD/.system}"
USERDIR="${USERDATA_PATH:-$SD/.userdata/tg5040}"
TARGET="$SYS/bin/nextui.elf"
STATE="$DIR/state"
ACTION="${1:---install}"
REQUESTED_STYLE="${2:-}"
mkdir -p "$STATE"
hash() { sha256sum "$1" | awk '{print $1}'; }
log() { printf '%s\n' "$*"; }
die() { log "ERROR: $*"; exit 1; }
command -v sha256sum >/dev/null 2>&1 || die "Missing sha256sum"
[ "${PLATFORM:-tg5040}" = tg5040 ] || die "Requires tg5040"
[ "${DEVICE:-}" = brickpro ] || die "Requires TrimUI Brick Pro"
[ -f "$SYS/version.txt" ] || die "Missing NX Redux version.txt"
IFS= read -r ROM_VERSION < "$SYS/version.txt" || :
case "$ROM_VERSION" in NXRedux-v*) ;; *) die "Not NX Redux; no modification" ;; esac
[ -f "$TARGET" ] && [ ! -L "$TARGET" ] || die "Launcher missing or symlink"
case "$ACTION" in --install|--boot|--restore|--status) ;; *) die "Unknown action" ;; esac
STYLE=mono
if [ -f "$STATE/style.txt" ]; then IFS= read -r STYLE < "$STATE/style.txt" || :; fi
case "$STYLE" in color|mono) ;; *) STYLE=mono ;; esac
if [ -n "$REQUESTED_STYLE" ]; then
    [ "$ACTION" = --install ] || die "Style selection is only available for --install"
    case "$REQUESTED_STYLE" in color|mono) STYLE="$REQUESTED_STYLE" ;; *) die "Style must be color or mono" ;; esac
fi
ICON_SOURCE="$DIR/icons/$STYLE"

if [ "$ACTION" = --status ]; then
    "$DIR/nxt-icon-engine" --probe "$TARGET"
    exit $?
fi

# One task at a time; recover a lock left by an interrupted boot.
LOCK="$STATE/lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    PID=$(cat "$LOCK/pid" 2>/dev/null || :)
    case "$PID" in ''|*[!0-9]*) die "Busy: invalid lock; check state/lock" ;; esac
    if kill -0 "$PID" 2>/dev/null; then log "BUSY: another repair is running"; exit 0; fi
    rm -f "$LOCK/pid"
    rmdir "$LOCK" || die "Cannot remove stale lock"
    mkdir "$LOCK" || die "Cannot acquire lock"
fi
printf '%s\n' "$$" > "$LOCK/pid"
STAGE="$TARGET.nxt-icons-$$"
cleanup() { rm -f "$STAGE"; rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || :; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

log "NXT Icon Repair 1.2 | $ROM_VERSION"
log "STYLE: $STYLE"
log "Launcher SHA256: $(hash "$TARGET")"
. "$DIR/app-icons.sh"

install_hook() {
    BOOT="$SYS/paks/MinUI.pak/launch.sh"
    AUTO="$USERDIR/auto.sh"
    if ! grep -Fq 'AUTO_PATH=$USERDATA_PATH/auto.sh' "$BOOT" 2>/dev/null || ! grep -Fq '"$AUTO_PATH"' "$BOOT" 2>/dev/null; then
        log "AUTO: boot contract unrecognised; manual repair remains available"
        return 0
    fi
    mkdir -p "$USERDIR"
    [ ! -L "$AUTO" ] || { log "AUTO: auto.sh is a symlink; left untouched"; return 0; }
    if [ -f "$AUTO" ] && grep -Fq '# BEGIN NXT ICON REPAIR 1' "$AUTO"; then
        log "AUTO: already enabled"
        return 0
    fi
    if [ -e "$AUTO" ]; then
        IFS= read -r FIRST < "$AUTO" || :
        case "$FIRST" in '#!/bin/sh'|'#!/bin/ash'|'#!/bin/bash') ;; *) log "AUTO: existing auto.sh is not a recognised shell script; left untouched"; return 0 ;; esac
        cp -p "$AUTO" "$STATE/auto.original.new"
        mv "$STATE/auto.original.new" "$STATE/auto.original"
        printf '1\n' > "$STATE/auto.existed"
    else
        FIRST='#!/bin/sh'
        printf '0\n' > "$STATE/auto.existed"
        rm -f "$STATE/auto.original"
    fi
    NEW="$AUTO.nxt-$$"
    {
        printf '%s\n' "$FIRST"
        cat "$DIR/boot-block.txt"
        [ ! -f "$AUTO" ] || tail -n +2 "$AUTO"
    } > "$NEW"
    sh -n "$NEW" || { rm -f "$NEW"; log "AUTO: syntax check failed"; return 0; }
    chmod 755 "$NEW"
    mv "$NEW" "$AUTO"
    hash "$AUTO" > "$STATE/auto.managed.sha256"
    log "AUTO: enabled; checks once per boot, no background daemon"
}

remove_hook() {
    AUTO="$USERDIR/auto.sh"
    if [ -f "$AUTO" ] && [ ! -L "$AUTO" ] && grep -Fq '# BEGIN NXT ICON REPAIR 1' "$AUTO"; then
        MANAGED=$(cat "$STATE/auto.managed.sha256" 2>/dev/null || :)
        if [ "$(hash "$AUTO")" = "$MANAGED" ]; then
            if [ "$(cat "$STATE/auto.existed" 2>/dev/null || :)" = 1 ] && [ -f "$STATE/auto.original" ]; then
                cp -p "$STATE/auto.original" "$AUTO.nxt-$$"
                mv "$AUTO.nxt-$$" "$AUTO"
            else rm -f "$AUTO"; fi
        else
            # Preserve user additions made since installation.
            awk '/^# BEGIN NXT ICON REPAIR 1$/{skip=1;next} /^# END NXT ICON REPAIR 1$/{skip=0;next} !skip{print}' "$AUTO" > "$AUTO.nxt-$$"
            sh -n "$AUTO.nxt-$$" || { rm -f "$AUTO.nxt-$$"; die "Cannot safely remove auto hook"; }
            chmod 755 "$AUTO.nxt-$$"; mv "$AUTO.nxt-$$" "$AUTO"
        fi
        log "AUTO: disabled; existing user script preserved"
    fi
}

legacy_installers() {
    MODE="$1"
    for APP in "$SD/Tools"/NXT*.pak "$SD/Tools/tg5040"/NXT*.pak; do
        OLD="$APP/integration/install-tools-icon.sh"
        [ -f "$OLD" ] && [ ! -L "$OLD" ] || continue
        SAVED="$OLD.nxt-before"
        if [ "$MODE" = restore ]; then
            if { cmp -s "$OLD" "$DIR/legacy-wrapper.sh" || cmp -s "$OLD" "$DIR/legacy-wrapper-v1.sh"; } && [ -f "$SAVED" ] && [ ! -L "$SAVED" ]; then
                EXPECT=$(cat "$SAVED.sha256" 2>/dev/null || :)
                [ "$(hash "$SAVED")" = "$EXPECT" ] || { log "LEGACY: backup checksum failed: $OLD"; continue; }
                cp -p "$SAVED" "$OLD.nxt-$$"; mv "$OLD.nxt-$$" "$OLD"
                log "LEGACY: restored $OLD"
            fi
        else
            cmp -s "$OLD" "$DIR/legacy-wrapper.sh" && continue
            # Do not wrap a different/unknown NXT repair wrapper.
            grep -Fq 'NXT_COMMON_ICON_WRAPPER' "$OLD" && continue
            cp -p "$OLD" "$SAVED.new"; mv "$SAVED.new" "$SAVED"
            hash "$SAVED" > "$SAVED.sha256"
            cp "$DIR/legacy-wrapper.sh" "$OLD.nxt-$$"; chmod 755 "$OLD.nxt-$$"; mv "$OLD.nxt-$$" "$OLD"
            log "LEGACY: old icon installer redirected to common repair: $OLD"
        fi
    done
}

if [ "$ACTION" = --restore ]; then
    remove_hook
    [ -n "${NXT_ICON_EMBEDDED:-}" ] || legacy_installers restore   # inside NXT Retro Tube: other apps' installers are left alone
    restore_app_icons
    PROBE=$("$DIR/nxt-icon-engine" --probe "$TARGET" 2>&1) || :
    case "$PROBE" in
        INSTALLED:*|UPGRADE:*)
            BEFORE=$(hash "$TARGET")
            "$DIR/nxt-icon-engine" --unpatch "$TARGET" "$STAGE"
            chmod 755 "$STAGE"
            [ "$(hash "$TARGET")" = "$BEFORE" ] || die "Launcher changed concurrently; restore cancelled"
            mv "$STAGE" "$TARGET"
            sync
            log "RESTORED: current ROM's pre-repair launcher; return to Tools or restart"
            ;;
        *) log "RESTORED: auto repair disabled; current ROM launcher kept (no old backup copied)" ;;
    esac
    exit 0
fi

# Check the shipped engine/assets before writing launcher or resource files.
(cd "$DIR" && sha256sum -c PAYLOAD_SHA256.txt) || die "Package incomplete or damaged"
PROBE=$("$DIR/nxt-icon-engine" --probe "$TARGET" 2>&1) || {
    log "$PROBE"
    log "UNSUPPORTED: ROM kept. Send result.txt and the current nextui.elf for adaptation."
    exit 2
}
log "$PROBE"
case "$PROBE" in
    READY:*|UPGRADE:*)
        BEFORE=$(hash "$TARGET")
        mkdir -p "$STATE/backups"
        BACKUP="$STATE/backups/$BEFORE.elf"
        if [ -e "$BACKUP" ]; then
            [ "$(hash "$BACKUP")" = "$BEFORE" ] || die "Backup checksum mismatch"
        else
            cp -p "$TARGET" "$BACKUP.new"
            [ "$(hash "$BACKUP.new")" = "$BEFORE" ] || die "Backup copy failed"
            mv "$BACKUP.new" "$BACKUP"
        fi
        "$DIR/nxt-icon-engine" --create "$TARGET" "$STAGE"
        "$DIR/nxt-icon-engine" --probe "$STAGE"
        chmod 755 "$STAGE"
        sync
        [ "$(hash "$TARGET")" = "$BEFORE" ] || die "ROM launcher changed concurrently; repair cancelled"
        mv "$STAGE" "$TARGET"
        ;;
    INSTALLED:*) : ;;
    *) die "Unexpected engine result" ;;
esac

mkdir -p "$SYS/res/menu"
for NAME in menu_icon_nxt.png menu_icon_nxt_tube.png menu_icon_nxt_retro_movie.png menu_icon_nxt_booknest.png menu_icon_nxt_bricktok.png menu_icon_nxt_file.png menu_icon_nxt_repair.png; do
    SOURCE_ICON="$ICON_SOURCE/$NAME"
    [ "$NAME" != menu_icon_nxt_repair.png ] || SOURCE_ICON="$DIR/icons/$NAME"
    if ! cmp -s "$SOURCE_ICON" "$SYS/res/menu/$NAME"; then
        cp "$SOURCE_ICON" "$SYS/res/menu/$NAME.nxt-$$"
        mv "$SYS/res/menu/$NAME.nxt-$$" "$SYS/res/menu/$NAME"
    fi
done
install_app_icons
[ -n "${NXT_ICON_EMBEDDED:-}" ] || legacy_installers install   # inside NXT Retro Tube: other apps' installers are left alone
[ "$ACTION" != --install ] || install_hook
if [ "$ACTION" = --install ]; then
    printf '%s\n' "$STYLE" > "$STATE/style.txt.new"
    mv "$STATE/style.txt.new" "$STATE/style.txt"
fi
sync
log "DONE: Music + Tube + Movie + BookNest + BrickTok + File icons repaired. Return to Tools or restart."

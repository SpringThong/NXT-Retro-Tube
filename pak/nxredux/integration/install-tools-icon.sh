#!/bin/sh
# NXT Retro Tube: Tools-menu icons for NX Redux (NXT Retro Tube + NXT Retro Music).
# The launcher maps tool names to icons in code, so this installs a launcher built
# from the matching NX Redux source with two more entries. Supported launchers:
#   v2.4.1  launcher built from tag v2.4.1 (nx-redux/nx-handheld) + 2 lines in tiles.c
#   v2.1.2  launcher built from tag v2.1.2 (older installs)
# Only a launcher of the same NX Redux version is replaced, and a backup is kept.
#   install-tools-icon.sh            install or refresh
#   install-tools-icon.sh --status   installed | ready | unsupported
#   install-tools-icon.sh --restore  put the previous launcher back
set -u
DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SDCARD="${SDCARD_PATH:-/mnt/SDCARD}"
SYSTEM="${SYSTEM_PATH:-$SDCARD/.system}"
TARGET="$SYSTEM/bin/nextui.elf"
MENU="$SYSTEM/res/menu"
# v2.1.2 (official launcher, NXT Retro Music's launcher, ours)
ORIGINAL_212=ded138923424aa6ed324382bfc1c1905af3989002ea7376afcd58d4da8c2720c
NXT_MUSIC_212=ea1f92faab6f6efc0df6fb6b483a41a7979e888bd1a532cba89fb1786dd5ad47
OURS_212=225d6e88bc565d932f64cf18be66bf718537ec6db550dbe897f517c80d84729e
# v2.4.1 (ours; the official one is recognised by version.txt + launcher markers)
OURS_241=d7902869c4ade0c865c37ff7eeb8d9f5fd76ff59773d969c6f2bca600f04566c
hash() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
say() { printf '%s\n' "$1"; }
command -v sha256sum >/dev/null 2>&1 || { say "Thiếu sha256sum, không cài được icon"; exit 1; }
[ -f "$TARGET" ] || { say "Không thấy launcher NX Redux"; exit 1; }
CUR=$(hash "$TARGET")
TAG=$(sed -n 3p "$SYSTEM/version.txt" 2>/dev/null | tr -d '\r ')
is_launcher() { grep -q "menu_icon_cheats.png" "$1" 2>/dev/null; }

# which bundled launcher fits this device
VER=""; NEW=""; OURS=""
case "$CUR" in
    "$OURS_241") VER=v2.4.1 ;;
    "$OURS_212"|"$ORIGINAL_212"|"$NXT_MUSIC_212") VER=v2.1.2 ;;
    *) [ "$TAG" = v2.4.1 ] && is_launcher "$TARGET" && VER=v2.4.1 ;;
esac
case "$VER" in
    v2.4.1) NEW="$DIR/nextui-v2.4.1-nxt-icons.elf"; OURS=$OURS_241 ;;
    v2.1.2) NEW="$DIR/nextui-v2.1.2-nxt-tube-icon.elf"; OURS=$OURS_212 ;;
esac
BACKUP="$DIR/backup/nextui-${VER:-unknown}-before-tube.elf"
[ "$VER" = v2.1.2 ] && [ ! -f "$BACKUP" ] && [ -f "$DIR/backup/nextui-before-tube.elf" ] && BACKUP="$DIR/backup/nextui-before-tube.elf"
backup_ok() {   # the backup is a launcher of this version that is not ours
    [ -f "$BACKUP" ] || return 1
    BH=$(hash "$BACKUP")
    case "$VER" in
        v2.1.2) [ "$BH" = "$ORIGINAL_212" ] || [ "$BH" = "$NXT_MUSIC_212" ] ;;
        v2.4.1) [ "$BH" != "$OURS_241" ] && is_launcher "$BACKUP" ;;
        *) return 1 ;;
    esac
}

if [ "${1:-}" = --status ]; then
    if [ -z "$VER" ]; then say unsupported
    elif [ "$CUR" = "$OURS" ]; then say installed
    else say ready; fi
    exit 0
fi
if [ "${1:-}" = --restore ]; then
    [ -n "$VER" ] && [ "$CUR" = "$OURS" ] || { say "Launcher hiện tại không phải bản có icon NXT"; exit 1; }
    backup_ok || { say "Không còn bản sao lưu launcher hợp lệ"; exit 1; }
    cp "$BACKUP" "$TARGET.tube-restore" && chmod 755 "$TARGET.tube-restore" && mv "$TARGET.tube-restore" "$TARGET" || { rm -f "$TARGET.tube-restore"; say "Không khôi phục được launcher"; exit 1; }
    rm -f "$MENU/menu_icon_nxt_tube.png"
    say "Đã khôi phục launcher gốc. Khởi động lại máy để áp dụng."
    exit 0
fi

[ -n "$VER" ] || { say "Bỏ qua: NX Redux ${TAG:-?} chưa được hỗ trợ (hỗ trợ v2.4.1 và v2.1.2)"; exit 2; }
mkdir -p "$MENU" "$DIR/backup"
cp "$DIR/menu_icon_nxt_tube.png" "$MENU/menu_icon_nxt_tube.png.tmp" && mv "$MENU/menu_icon_nxt_tube.png.tmp" "$MENU/menu_icon_nxt_tube.png"
# NXT Retro Music's icon too (the launcher maps both), unless it is already there
[ -f "$MENU/menu_icon_nxt.png" ] || { cp "$DIR/menu_icon_nxt.png" "$MENU/menu_icon_nxt.png.tmp" && mv "$MENU/menu_icon_nxt.png.tmp" "$MENU/menu_icon_nxt.png"; }
if [ "$CUR" = "$OURS" ]; then say "Icon NXT Retro Tube đã có trong menu Tools (NX Redux $VER)"; exit 0; fi
[ "$(hash "$NEW")" = "$OURS" ] || { say "Tệp launcher kèm theo bị hỏng"; exit 1; }
# every library the new launcher links must resolve here, the same as the current one
LIBS="$SYSTEM/lib:${LD_LIBRARY_PATH:-}"
MISSING_NEW=$(LD_LIBRARY_PATH="$LIBS" LD_TRACE_LOADED_OBJECTS=1 "$NEW" 2>&1 | grep -c "not found")
MISSING_CUR=$(LD_LIBRARY_PATH="$LIBS" LD_TRACE_LOADED_OBJECTS=1 "$TARGET" 2>&1 | grep -c "not found")
if [ "${MISSING_NEW:-0}" -gt "${MISSING_CUR:-0}" ]; then say "Bỏ qua: launcher mới thiếu thư viện trên máy này"; exit 1; fi
if ! backup_ok; then
    cp "$TARGET" "$BACKUP.tmp" && mv "$BACKUP.tmp" "$BACKUP" || { say "Không sao lưu được launcher"; exit 1; }
fi
cp "$NEW" "$TARGET.tube-install" && chmod 755 "$TARGET.tube-install" || { rm -f "$TARGET.tube-install"; say "Không chép được launcher"; exit 1; }
[ "$(hash "$TARGET.tube-install")" = "$OURS" ] || { rm -f "$TARGET.tube-install"; say "Chép launcher bị lỗi"; exit 1; }
mv "$TARGET.tube-install" "$TARGET"
say "Đã cài icon NXT Retro Tube (NX Redux $VER). Mở lại Tools hoặc khởi động lại máy để thấy icon."

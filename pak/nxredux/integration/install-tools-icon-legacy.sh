#!/bin/sh
# NXT Retro Tube: Tools-menu icons for NX Redux (NXT Retro Tube + NXT Retro Music), any version.
# The launcher maps tool names to icons with a fixed table in its code. nxt-iconpatch copies that
# table with our two entries added into the launcher the device already has (whatever NX Redux
# version it is), so a system update that brings a new launcher only needs this run again
# (launch.sh does it at every start). The launcher it replaces is kept in backup/.
#   install-tools-icon.sh            install or refresh
#   install-tools-icon.sh --status   installed | ready | unsupported
#   install-tools-icon.sh --restore  put the previous launcher back
set -u
DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SDCARD="${SDCARD_PATH:-/mnt/SDCARD}"
SYSTEM="${SYSTEM_PATH:-$SDCARD/.system}"
TARGET="$SYSTEM/bin/nextui.elf"
MENU="$SYSTEM/res/menu"
PATCHER="$DIR/nxt-iconpatch"
# launchers with our icons from earlier releases (prebuilt v2.1.2 / v2.4.1)
OURS_212=225d6e88bc565d932f64cf18be66bf718537ec6db550dbe897f517c80d84729e
OURS_241=d7902869c4ade0c865c37ff7eeb8d9f5fd76ff59773d969c6f2bca600f04566c
hash() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
say() { printf '%s\n' "$1"; }
command -v sha256sum >/dev/null 2>&1 || { say "Thiếu sha256sum, không cài được icon"; exit 1; }
[ -f "$TARGET" ] || { say "Không thấy launcher NX Redux"; exit 1; }
chmod +x "$PATCHER" 2>/dev/null || :
CUR=$(hash "$TARGET")
TAG=$(sed -n 3p "$SYSTEM/version.txt" 2>/dev/null | tr -d '\r ')
STATE=$("$PATCHER" --check "$TARGET" 2>/dev/null | head -n 1)
[ -n "$STATE" ] || STATE="unsupported: no patcher"
mkdir -p "$DIR/backup"

icons() {
    mkdir -p "$MENU"
    cp "$DIR/menu_icon_nxt_tube.png" "$MENU/menu_icon_nxt_tube.png.tmp" && mv "$MENU/menu_icon_nxt_tube.png.tmp" "$MENU/menu_icon_nxt_tube.png"
    # NXT Retro Music's icon too (the patched table maps both), unless it is already there
    [ -f "$MENU/menu_icon_nxt.png" ] || { cp "$DIR/menu_icon_nxt.png" "$MENU/menu_icon_nxt.png.tmp" && mv "$MENU/menu_icon_nxt.png.tmp" "$MENU/menu_icon_nxt.png"; }
}

if [ "${1:-}" = --status ]; then
    case "$STATE" in
        patched) icons 2>/dev/null; say installed ;;
        patchable) say ready ;;
        *) say unsupported ;;
    esac
    exit 0
fi

if [ "${1:-}" = --restore ]; then
    [ "$STATE" = patched ] || { say "Launcher hiện tại không có icon NXT"; exit 1; }
    B=""
    # the launcher we patched last time (recorded with the hash of the result)
    if [ -f "$DIR/backup/patched.sha256" ] && [ "$(cat "$DIR/backup/patched.sha256")" = "$CUR" ] && [ -f "$DIR/backup/nextui-original.elf" ]; then B="$DIR/backup/nextui-original.elf"; fi
    # backups of the prebuilt launchers of older releases
    [ -z "$B" ] && [ "$CUR" = "$OURS_241" ] && [ -f "$DIR/backup/nextui-v2.4.1-before-tube.elf" ] && B="$DIR/backup/nextui-v2.4.1-before-tube.elf"
    [ -z "$B" ] && [ "$CUR" = "$OURS_212" ] && for F in "$DIR/backup/nextui-v2.1.2-before-tube.elf" "$DIR/backup/nextui-before-tube.elf"; do [ -f "$F" ] && { B="$F"; break; }; done
    [ -n "$B" ] || { say "Không còn bản sao lưu launcher hợp lệ"; exit 1; }
    "$PATCHER" --check "$B" 2>/dev/null | grep -q '^patched' && { say "Bản sao lưu cũng đã có icon, bỏ qua"; exit 1; }
    cp "$B" "$TARGET.tube-restore" && chmod 755 "$TARGET.tube-restore" && mv "$TARGET.tube-restore" "$TARGET" || { rm -f "$TARGET.tube-restore"; say "Không khôi phục được launcher"; exit 1; }
    rm -f "$MENU/menu_icon_nxt_tube.png" "$DIR/backup/patched.sha256"
    say "Đã khôi phục launcher gốc. Khởi động lại máy để áp dụng."
    exit 0
fi

case "$STATE" in
    patched) icons; say "Icon NXT Retro Tube đã có trong menu Tools (NX Redux ${TAG:-?})"; exit 0 ;;
    patchable) ;;
    *) say "Bỏ qua: launcher NX Redux ${TAG:-?} chưa sửa được (${STATE#unsupported: })"; exit 2 ;;
esac
icons
rm -f "$TARGET.tube-install"
OUT=$("$PATCHER" "$TARGET" "$TARGET.tube-install" 2>&1) || { rm -f "$TARGET.tube-install"; say "Không sửa được launcher: $OUT"; exit 1; }
"$PATCHER" --check "$TARGET.tube-install" 2>/dev/null | grep -q '^patched' || { rm -f "$TARGET.tube-install"; say "Launcher sửa xong không đúng, bỏ qua"; exit 1; }
chmod 755 "$TARGET.tube-install"
# every library the patched launcher links must resolve here, the same as the current one
LIBS="$SYSTEM/lib:${LD_LIBRARY_PATH:-}"
# (this also makes the kernel and ld.so load the patched file once: it must load cleanly)
TRACE=$(LD_LIBRARY_PATH="$LIBS" LD_TRACE_LOADED_OBJECTS=1 "$TARGET.tube-install" 2>&1); RC=$?
case "$TRACE" in *libc.so*) ;; *) RC=1 ;; esac
if [ "$RC" != 0 ]; then rm -f "$TARGET.tube-install"; say "Bỏ qua: launcher sửa xong không nạp được trên máy này"; exit 1; fi
MISSING_NEW=$(printf '%s\n' "$TRACE" | grep -c "not found")
MISSING_CUR=$(LD_LIBRARY_PATH="$LIBS" LD_TRACE_LOADED_OBJECTS=1 "$TARGET" 2>&1 | grep -c "not found")
if [ "${MISSING_NEW:-0}" -gt "${MISSING_CUR:-0}" ]; then rm -f "$TARGET.tube-install"; say "Bỏ qua: launcher sửa xong không chạy được trên máy này"; exit 1; fi
cp "$TARGET" "$DIR/backup/nextui-original.elf.tmp" && mv "$DIR/backup/nextui-original.elf.tmp" "$DIR/backup/nextui-original.elf" || { rm -f "$TARGET.tube-install"; say "Không sao lưu được launcher"; exit 1; }
echo "${TAG:-?}" > "$DIR/backup/nextui-original.version"
hash "$TARGET.tube-install" > "$DIR/backup/patched.sha256"
mv "$TARGET.tube-install" "$TARGET" || { say "Không thay được launcher"; exit 1; }
sync
say "Đã cài icon NXT Retro Tube (NX Redux ${TAG:-?}). Thoát app hoặc khởi động lại máy để thấy icon."

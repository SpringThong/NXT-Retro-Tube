#!/bin/sh
# NXT_COMMON_ICON_WRAPPER 1 - old per-app installers must not replace the new launcher.
SD="${SDCARD_PATH:-/mnt/SDCARD}"
COMMON="$SD/Tools/NXT Icon Repair.pak/repair.sh"
if [ ! -f "$COMMON" ]; then
    case "${1:-}" in --status) echo unsupported ;; *) echo "NXT Icon Repair missing; use NXT Icon Restore before removing the repair tool." ;; esac
    exit 2
fi
case "${1:-}" in
    --status)
        RESULT=$(sh "$COMMON" --status 2>&1) || { echo unsupported; exit 0; }
        case "$RESULT" in INSTALLED:*) echo installed ;; READY:*) echo ready ;; *) echo unsupported ;; esac
        ;;
    --restore) exec sh "$COMMON" --restore ;;
    *) exec sh "$COMMON" --boot ;;
esac

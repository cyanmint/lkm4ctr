#!/system/bin/sh
MODDIR=${0%/*}
[ "$MODDIR" = "$0" ] && MODDIR=.
BUSYBOX=${LKM4CTR_BUSYBOX:-/data/adb/ksu/bin/busybox}

printf '%s\n' 'Press Volume Up to toggle loading/unloading lkm4ctr; press Volume Down to toggle boot auto-load.'
printf '%s\n' 'Waiting 30 seconds. If volume-key input is unavailable or no key is detected, Volume Up is selected by default.'

selection=up
if [ -x /system/bin/getevent ] && [ -x "$BUSYBOX" ]; then
    detected=$("$BUSYBOX" timeout 30 /system/bin/getevent -ql 2>/dev/null |
        "$BUSYBOX" awk '
            /KEY_VOLUMEUP[[:space:]]+DOWN/ { print "up"; exit }
            /KEY_VOLUMEDOWN[[:space:]]+DOWN/ { print "down"; exit }
        ')
    case "$detected" in
        up|down) selection=$detected ;;
        *) echo 'Volume-key input was unavailable; defaulting to Volume Up.' ;;
    esac
else
    echo 'Volume-key input is unavailable; defaulting to Volume Up.'
fi

case "$selection" in
    down)
        "$MODDIR/control.sh" toggle-autoload
        ;;
    up|"")
        "$MODDIR/control.sh" toggle-load
        ;;
    *)
        echo "Unrecognized volume-key event: $selection" >&2
        exit 1
        ;;
esac

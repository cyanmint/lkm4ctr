#!/system/bin/sh
MODDIR=${0%/*}
[ "$MODDIR" = "$0" ] && MODDIR=.
"$MODDIR/control.sh" boot

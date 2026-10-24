#!/system/bin/sh
# Load the module if post-fs-data did not, then start sync-tool: it watches HMA's config,
# evaluates it and pushes the resulting policy over netlink, forever.
MODDIR=${0%/*}
KO="$MODDIR/ko/tosya.ko"
LOG="$MODDIR/state/sync.log"
mkdir -p "$MODDIR/state"

if [ -f "$KO" ] && ! lsmod | grep -q '^tosya '; then
  if [ -x /data/adb/ksud ]; then
    /data/adb/ksud insmod "$KO" >>"$LOG" 2>&1
    echo "[load] ksud insmod rc=$? $(date '+%m-%d %H:%M:%S')" >>"$LOG"
  fi
  if ! lsmod | grep -q '^tosya ' && [ -x "$MODDIR/lkmloader" ]; then
    "$MODDIR/lkmloader" "$KO" >>"$LOG" 2>&1
    echo "[load] lkmloader rc=$? $(date '+%m-%d %H:%M:%S')" >>"$LOG"
  fi
fi

[ -x "$MODDIR/sync-tool" ] && "$MODDIR/sync-tool" >>"$LOG" 2>&1 &

#!/system/bin/sh
# Load the module before the framework starts: the reads a child does before its window
# closes have to be covered. ksud is what an installed KernelSU already has; the bundled
# lkmloader is for a device without one -- it needs no root solution to be installed, and it
# is what resolves the ko's undefined symbols from /proc/kallsyms and, when the kernel names
# the vermagic it wants, writes that in and retries.
MODDIR=${0%/*}
KO="$MODDIR/ko/tosya.ko"
LOG="$MODDIR/state/sync.log"
mkdir -p "$MODDIR/state"
[ -f "$KO" ] || exit 0
lsmod | grep -q '^tosya ' && exit 0

if [ -x /data/adb/ksud ]; then
  /data/adb/ksud insmod "$KO" >>"$LOG" 2>&1
  echo "[load] ksud insmod rc=$? $(date '+%m-%d %H:%M:%S')" >>"$LOG"
fi
lsmod | grep -q '^tosya ' && exit 0

[ -x "$MODDIR/lkmloader" ] || { echo "[load] no lkmloader" >>"$LOG"; exit 0; }
"$MODDIR/lkmloader" "$KO" >>"$LOG" 2>&1
_rc=$?
echo "[load] lkmloader rc=$_rc $(date '+%m-%d %H:%M:%S')" >>"$LOG"
lsmod | grep -q '^tosya ' || echo "[load] not loaded; no ko for this kernel? see the customize.sh note" >>"$LOG"

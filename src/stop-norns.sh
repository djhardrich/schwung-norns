#!/bin/sh
# stop-norns.sh — Stop Norns stack in the chroot
# Called by pw-helper: stop-norns.sh <slot>
SLOT="${1:-1}"
CHROOT="/data/UserData/pw-chroot"
PID_DIR="/tmp/norns-pids-${SLOT}"

# Kill by process name (PID files are often stale or missing)
for proc in maiden norns-input-bridge midi-bridge matron ws-wrapper crone scsynth sclang jackd; do
    pkill -f "$proc" 2>/dev/null || true
done
sleep 1
# Force kill survivors
for proc in matron crone scsynth sclang jackd; do
    pkill -9 -f "$proc" 2>/dev/null || true
done

# Clean up session dbus (system dbus can stay)
chroot "$CHROOT" sh -c "pkill -u 1000 dbus-daemon 2>/dev/null || true"

# Clean up PID files only — FIFOs are managed by the DSP plugin
rm -rf "$PID_DIR"

echo "Norns stopped (slot $SLOT)"

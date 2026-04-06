#!/bin/sh
# stop-norns.sh — Stop Norns stack in the chroot
# Called by pw-helper: stop-norns.sh <slot>
SLOT="${1:-1}"
CHROOT="/data/UserData/pw-chroot"
PID_DIR="/tmp/norns-pids-${SLOT}"

# Kill JACK clients first (they hang if jackd dies before them),
# then jackd last.  norns-input-bridge ignores SIGTERM when stuck
# in a JACK call, so it always gets SIGKILL in the second pass.
for proc in maiden norns-input-bridge midi-bridge matron ws-wrapper crone scsynth sclang; do
    pkill -f "$proc" 2>/dev/null || true
    pids=$(pidof "$proc" 2>/dev/null) && kill $pids 2>/dev/null || true
done
sleep 0.5
# Force kill JACK clients, then stop jackd
for proc in norns-input-bridge midi-bridge matron crone scsynth sclang; do
    pkill -9 -f "$proc" 2>/dev/null || true
    pids=$(pidof "$proc" 2>/dev/null) && kill -9 $pids 2>/dev/null || true
done
# Now kill jackd (clients are gone so it shuts down cleanly)
pkill -f jackd 2>/dev/null || true
pids=$(pidof jackd 2>/dev/null) && kill $pids 2>/dev/null || true
sleep 0.5
pids=$(pidof jackd 2>/dev/null) && kill -9 $pids 2>/dev/null || true

# Clean up session dbus (system dbus can stay)
chroot "$CHROOT" sh -c "pkill -u 1000 dbus-daemon 2>/dev/null || true"

# Clean up PID files only — FIFOs are managed by the DSP plugin
rm -rf "$PID_DIR"

echo "Norns stopped (slot $SLOT)"

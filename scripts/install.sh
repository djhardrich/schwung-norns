#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="norns"
# Replace move.local with your device's IP address if mDNS is not available
DEVICE_HOST="${DEVICE_HOST:-move.local}"
USER_SSH="ableton@$DEVICE_HOST"
ROOT_SSH="root@$DEVICE_HOST"
REMOTE_MODULE="/data/UserData/move-anything/modules/tools/$MODULE_ID"
REMOTE_CHROOT="/data/UserData/pw-chroot"
DIST_DIR="$REPO_ROOT/dist/$MODULE_ID"

echo "=== Installing Norns Module ==="
echo "Device: $DEVICE_HOST"
echo ""

# ── Install module files ──
if [ ! -d "$DIST_DIR" ]; then
    echo "Error: $DIST_DIR not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "--- Deploying module to $REMOTE_MODULE ---"
ssh "$USER_SSH" "mkdir -p $REMOTE_MODULE"
scp -r "$DIST_DIR/"* "$USER_SSH:$REMOTE_MODULE/"
ssh "$USER_SSH" "chmod +x $REMOTE_MODULE/start-norns.sh $REMOTE_MODULE/stop-norns.sh $REMOTE_MODULE/restart-norns.sh"

# ── Install pw-helper (setuid root) ──
PW_HELPER="$REPO_ROOT/build/pw-helper"
if [ -f "$PW_HELPER" ]; then
    echo ""
    echo "--- Installing pw-helper (setuid root) ---"
    ssh "$ROOT_SSH" "mkdir -p /data/UserData/move-anything/bin"
    scp "$PW_HELPER" "$ROOT_SSH:/data/UserData/move-anything/bin/pw-helper-norns"
    ssh "$ROOT_SSH" "chown root:root /data/UserData/move-anything/bin/pw-helper-norns && chmod 4755 /data/UserData/move-anything/bin/pw-helper-norns"
    echo "pw-helper-norns installed"
fi

# ── Install norns-input-bridge to chroot ──
INPUT_BRIDGE="$REPO_ROOT/build/norns-input-bridge"
if [ -f "$INPUT_BRIDGE" ]; then
    echo ""
    echo "--- Installing norns-input-bridge to chroot ---"
    ssh "$ROOT_SSH" "mkdir -p $REMOTE_CHROOT/usr/local/bin"
    scp "$INPUT_BRIDGE" "$ROOT_SSH:$REMOTE_CHROOT/usr/local/bin/norns-input-bridge"
    ssh "$ROOT_SSH" "chmod +x $REMOTE_CHROOT/usr/local/bin/norns-input-bridge"
    echo "norns-input-bridge installed"
fi

# ── Install jack-fifo-bridge to chroot ──
JACK_BRIDGE="$REPO_ROOT/build/jack-fifo-bridge"
if [ -f "$JACK_BRIDGE" ]; then
    echo ""
    echo "--- Installing jack-fifo-bridge to chroot ---"
    ssh "$ROOT_SSH" "mkdir -p $REMOTE_CHROOT/usr/local/bin"
    scp "$JACK_BRIDGE" "$ROOT_SSH:$REMOTE_CHROOT/usr/local/bin/jack-fifo-bridge"
    ssh "$ROOT_SSH" "chmod +x $REMOTE_CHROOT/usr/local/bin/jack-fifo-bridge"
    echo "jack-fifo-bridge installed"
fi

if [ ! -f "$PW_HELPER" ]; then
    echo ""
    echo "NOTE: pw-helper not found. Norns must be started manually as root."
fi

# ── Install chroot profile ──
echo ""
echo "--- Installing chroot profile ---"
ssh "$ROOT_SSH" "mkdir -p $REMOTE_CHROOT/etc/profile.d && cat > $REMOTE_CHROOT/etc/profile.d/pipewire.sh << 'PROFEOF'
# Auto-set PipeWire environment for Move bridge
export XDG_RUNTIME_DIR=/tmp/pw-runtime-1
export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/pw-runtime-1/dbus-pw
PROFEOF
chmod 644 $REMOTE_CHROOT/etc/profile.d/pipewire.sh"

# ── Install PipeWire audio config (rate, quantum, no-RT) ──
echo ""
echo "--- Installing PipeWire config ---"
ssh "$ROOT_SSH" "mkdir -p $REMOTE_CHROOT/etc/pipewire/pipewire.conf.d
rm -f $REMOTE_CHROOT/etc/pipewire/pipewire.conf.d/no-rt.conf
cat > $REMOTE_CHROOT/etc/pipewire/pipewire.conf.d/move-audio.conf << 'PWEOF'
context.properties = {
    module.rt                   = false
    default.clock.rate          = 44100
    default.clock.allowed-rates = [ 44100 ]
    default.clock.quantum       = 1024
    default.clock.min-quantum   = 1024
    default.clock.max-quantum   = 1024
}
PWEOF
chmod 644 $REMOTE_CHROOT/etc/pipewire/pipewire.conf.d/move-audio.conf
mkdir -p $REMOTE_CHROOT/etc/wireplumber/wireplumber.conf.d
cp $REMOTE_CHROOT/etc/pipewire/pipewire.conf.d/move-audio.conf $REMOTE_CHROOT/etc/wireplumber/wireplumber.conf.d/move-audio.conf
mkdir -p $REMOTE_CHROOT/etc/security/limits.d
echo '# Disabled - RT scheduling conflicts with Move audio engine' > $REMOTE_CHROOT/etc/security/limits.d/25-pw-rlimits.conf"

# ── Deploy patches and setup scripts ──
echo ""
echo "--- Deploying patches and setup scripts ---"
ssh "$ROOT_SSH" "mkdir -p $REMOTE_MODULE/patches"
scp "$REPO_ROOT/patches/apply-move-patches.sh" "$ROOT_SSH:$REMOTE_MODULE/patches/"
scp "$REPO_ROOT/scripts/setup-norns.sh" "$ROOT_SSH:/data/setup-norns.sh"
scp "$REPO_ROOT/scripts/package-norns-chroot.sh" "$ROOT_SSH:/data/package-norns-chroot.sh"
ssh "$ROOT_SSH" "chmod +x $REMOTE_MODULE/patches/apply-move-patches.sh /data/setup-norns.sh /data/package-norns-chroot.sh"

# ── Check if Norns is installed ──
echo ""
if ssh "$ROOT_SSH" "[ -d $REMOTE_CHROOT/home/we/norns ]" 2>/dev/null; then
    echo "Norns detected in chroot."
else
    echo "NOTE: Norns not yet installed in chroot."
    echo "  Run: ssh root@$DEVICE_HOST 'sh /data/setup-norns.sh'"
fi

echo ""
echo "=== Install Complete ==="
echo "Module: $REMOTE_MODULE"
echo "Chroot: $REMOTE_CHROOT"
echo ""
echo "Load 'Norns' from the Tools menu."
echo ""
echo "Controls:"
echo "  Knobs 1-3       = Norns E1/E2/E3"
echo "  Knob 4/5/6 taps = Norns K1/K2/K3"
echo "  Knob 7 tap      = Restart Norns"
echo "  Shift+Vol+Jog   = Exit to Shadow UI"
echo ""
echo "Maiden: http://$DEVICE_HOST:5000"

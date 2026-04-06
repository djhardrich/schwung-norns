#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="norns"
DEVICE_HOST="${DEVICE_HOST:-move.local}"
REMOTE_MODULE="/data/UserData/schwung/modules/tools/$MODULE_ID"
REMOTE_CHROOT="/data/UserData/pw-chroot"
DIST_DIR="$REPO_ROOT/dist/$MODULE_ID"

echo "=== Installing Norns Module ==="
echo "Device: $DEVICE_HOST"
echo ""

# ── Install module files ──
if [ ! -d "$DIST_DIR" ]; then
    # Look for a release tarball to extract
    TARBALL=""
    for f in "$REPO_ROOT/dist/"*norns*.tar.gz; do
        [ -f "$f" ] && TARBALL="$f" && break
    done
    if [ -n "$TARBALL" ]; then
        echo "Extracting $(basename "$TARBALL") ..."
        tar -xzf "$TARBALL" -C "$REPO_ROOT/dist/"
    fi
    if [ ! -d "$DIST_DIR" ]; then
        echo "Error: $DIST_DIR not found."
        echo "Either run ./scripts/build.sh or place a release tar.gz in dist/"
        exit 1
    fi
fi

echo "--- Deploying module to $REMOTE_MODULE ---"
ssh "root@$DEVICE_HOST" "mkdir -p $REMOTE_MODULE"
scp -r "$DIST_DIR/"* "root@$DEVICE_HOST:$REMOTE_MODULE/"
ssh "root@$DEVICE_HOST" "chmod +x $REMOTE_MODULE/norns $REMOTE_MODULE/start-norns.sh $REMOTE_MODULE/stop-norns.sh $REMOTE_MODULE/restart-norns.sh && chown -R ableton:users $REMOTE_MODULE"

# ── Install pw-helper (setuid root) ──
PW_HELPER="$REPO_ROOT/build/pw-helper"
[ ! -f "$PW_HELPER" ] && PW_HELPER="$DIST_DIR/bin/pw-helper"
if [ -f "$PW_HELPER" ]; then
    echo ""
    echo "--- Installing pw-helper (setuid root) ---"
    ssh "root@$DEVICE_HOST" "mkdir -p /data/UserData/schwung/bin"
    scp "$PW_HELPER" "root@$DEVICE_HOST:/data/UserData/schwung/bin/pw-helper-norns"
    ssh "root@$DEVICE_HOST" "chown root:root /data/UserData/schwung/bin/pw-helper-norns && chmod 4755 /data/UserData/schwung/bin/pw-helper-norns"
    echo "pw-helper-norns installed"
fi

# ── Install norns-input-bridge to chroot ──
INPUT_BRIDGE="$REPO_ROOT/build/norns-input-bridge"
[ ! -f "$INPUT_BRIDGE" ] && INPUT_BRIDGE="$DIST_DIR/bin/norns-input-bridge"
if [ -f "$INPUT_BRIDGE" ]; then
    echo ""
    echo "--- Installing norns-input-bridge to chroot ---"
    ssh "root@$DEVICE_HOST" "mkdir -p $REMOTE_CHROOT/usr/local/bin"
    scp "$INPUT_BRIDGE" "root@$DEVICE_HOST:$REMOTE_CHROOT/usr/local/bin/norns-input-bridge"
    ssh "root@$DEVICE_HOST" "chmod +x $REMOTE_CHROOT/usr/local/bin/norns-input-bridge"
    echo "norns-input-bridge installed"
fi


if [ ! -f "$PW_HELPER" ]; then
    echo ""
    echo "NOTE: pw-helper not found. Norns must be started manually as root."
fi

# ── Install chroot profile ──
echo ""
echo "--- Installing chroot profile ---"
ssh "root@$DEVICE_HOST" "mkdir -p $REMOTE_CHROOT/etc/profile.d && cat > $REMOTE_CHROOT/etc/profile.d/jack.sh << 'PROFEOF'
# Auto-set JACK environment for Move
export XDG_RUNTIME_DIR=/tmp/pw-runtime-1
PROFEOF
chmod 644 $REMOTE_CHROOT/etc/profile.d/jack.sh
# Clean up old PipeWire profile if present
rm -f $REMOTE_CHROOT/etc/profile.d/pipewire.sh"

# ── Deploy patches and setup scripts ──
echo ""
echo "--- Deploying patches and setup scripts ---"
ssh "root@$DEVICE_HOST" "mkdir -p $REMOTE_MODULE/patches"
scp "$REPO_ROOT/patches/apply-move-patches.sh" "root@$DEVICE_HOST:$REMOTE_MODULE/patches/"
scp "$REPO_ROOT/scripts/setup-norns.sh" "root@$DEVICE_HOST:/data/setup-norns.sh"
scp "$REPO_ROOT/scripts/package-norns-chroot.sh" "root@$DEVICE_HOST:/data/package-norns-chroot.sh"
ssh "root@$DEVICE_HOST" "mkdir -p $REMOTE_MODULE/scripts"
scp "$REPO_ROOT/scripts/build-sc-plugins.sh" "root@$DEVICE_HOST:$REMOTE_MODULE/scripts/build-sc-plugins.sh"
ssh "root@$DEVICE_HOST" "chmod +x $REMOTE_MODULE/patches/apply-move-patches.sh $REMOTE_MODULE/scripts/build-sc-plugins.sh /data/setup-norns.sh /data/package-norns-chroot.sh"

# ── Check if Norns is installed ──
echo ""
if ssh "root@$DEVICE_HOST" "[ -d $REMOTE_CHROOT/home/we/norns ]" 2>/dev/null; then
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

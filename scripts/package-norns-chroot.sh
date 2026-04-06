#!/bin/sh
# package-norns-chroot.sh — Build and package pre-built norns binaries
# Run on the Move: sh /data/package-norns-chroot.sh
# Produces: /data/UserData/norns-move-prebuilt.tar.gz
#
# This script:
#   1. Resets norns source to upstream
#   2. Applies Move patches (FIFO screen/input/MIDI/grid, device stubs)
#   3. Does a clean build of matron, crone, ws-wrapper, etc.
#   4. Builds/verifies 64-bit SC plugins
#   5. Packages binaries + Lua core + SC engines + Maiden into a tarball
#
# Upload the tarball to GitHub Releases. End users run setup-norns.sh
# which downloads this instead of building from source (~90 minutes saved).
set -e

CHROOT="/data/UserData/pw-chroot"
MODULE_DIR="/data/UserData/schwung/modules/tools/norns"
OUT="/data/UserData/norns-move-prebuilt.tar.gz"

echo "=== Building and packaging norns for Move ==="

# Ensure chroot mounts
for fs in proc sys dev dev/pts tmp; do
    case "$fs" in
        proc)    mountpoint -q "$CHROOT/proc"    2>/dev/null || mount -t proc proc "$CHROOT/proc" ;;
        sys)     mountpoint -q "$CHROOT/sys"     2>/dev/null || mount -t sysfs sys "$CHROOT/sys" ;;
        dev)     mountpoint -q "$CHROOT/dev"     2>/dev/null || mount --bind /dev "$CHROOT/dev" ;;
        dev/pts) mountpoint -q "$CHROOT/dev/pts" 2>/dev/null || mount --bind /dev/pts "$CHROOT/dev/pts" ;;
        tmp)     mountpoint -q "$CHROOT/tmp"     2>/dev/null || mount --bind /tmp "$CHROOT/tmp" ;;
    esac
done

# Ensure git safe.directory is set (write directly — git config itself can
# fail with "not a git repository" due to directory ownership checks)
mkdir -p "$CHROOT/home/we/.config/git"
if ! grep -q '/home/we/norns' "$CHROOT/home/we/.config/git/config" 2>/dev/null; then
    cat >> "$CHROOT/home/we/.config/git/config" << 'GITEOF'
[safe]
	directory = /home/we/norns
GITEOF
fi
chown -R 1000:1000 "$CHROOT/home/we/.config/git"

# Step 1: Reset norns source to upstream
echo ""
echo "--- Resetting norns source ---"
chrt -o 0 chroot "$CHROOT" su - move -c '
    cd /home/we/norns
    git checkout -- matron/src/ crone/src/ wscript matron/wscript
'

# Step 2: Apply Move patches
echo ""
echo "--- Applying Move patches ---"
if [ -f "$MODULE_DIR/patches/apply-move-patches.sh" ]; then
    mkdir -p "$CHROOT/home/we/norns/patches"
    cp "$MODULE_DIR/patches/apply-move-patches.sh" "$CHROOT/home/we/norns/patches/"
else
    echo "ERROR: apply-move-patches.sh not found at $MODULE_DIR/patches/" >&2
    exit 1
fi
chrt -o 0 chroot "$CHROOT" su - move -c "cd /home/we/norns && sh patches/apply-move-patches.sh"

# Step 3: Clean build
echo ""
echo "--- Clean build ---"
chrt -o 0 chroot "$CHROOT" su - move -c \
    "cd /home/we/norns && python3 waf clean && python3 waf build"

# Step 4: Verify binaries
echo ""
echo "--- Verifying binaries ---"
for bin in \
    "$CHROOT/home/we/norns/build/matron/matron" \
    "$CHROOT/home/we/norns/build/crone/crone" \
    "$CHROOT/home/we/norns/build/ws-wrapper/ws-wrapper"; do
    if [ ! -f "$bin" ]; then
        echo "ERROR: Missing $bin" >&2
        exit 1
    fi
    echo "  OK: $bin"
done

# Step 5: Build Maiden web UI
echo ""
echo "--- Building Maiden web UI ---"
chroot "$CHROOT" npm install -g yarn
chrt -o 0 chroot "$CHROOT" su - move -c '
    cd /home/we/maiden/web
    yarn install
    yarn build
    mkdir -p /home/we/maiden/app
    cp -r build /home/we/maiden/app/
'
if [ ! -d "$CHROOT/home/we/maiden/app/build" ]; then
    echo "ERROR: Maiden web UI build failed — app/build/ not found" >&2
    exit 1
fi
echo "  OK: maiden/app/build/"

# Step 6: Build or verify 64-bit SC plugins
echo ""
echo "--- Ensuring 64-bit SC plugins ---"
# Remove any 32-bit plugins first
chroot "$CHROOT" sh -c '
    for f in $(find /home/we/.local/share/SuperCollider/Extensions -name "*.so" 2>/dev/null); do
        case "$(file -b "$f")" in *32-bit*) rm -f "$f"; echo "  removed 32-bit: $f" ;; esac
    done
'

# Build 64-bit plugins if not already present
SC_PLUGIN_COUNT=$(chroot "$CHROOT" sh -c \
    'find /home/we/.local/share/SuperCollider/Extensions -name "*.so" 2>/dev/null | wc -l')
if [ "$SC_PLUGIN_COUNT" -lt 10 ]; then
    echo "  Only $SC_PLUGIN_COUNT .so files found, building plugins..."
    if [ -f "$MODULE_DIR/scripts/build-sc-plugins.sh" ]; then
        cp "$MODULE_DIR/scripts/build-sc-plugins.sh" "$CHROOT/tmp/"
        chrt -o 0 chroot "$CHROOT" su - move -c "sh /tmp/build-sc-plugins.sh"
        rm -f "$CHROOT/tmp/build-sc-plugins.sh"
    else
        echo "WARN: build-sc-plugins.sh not found, skipping plugin build" >&2
    fi
else
    echo "  $SC_PLUGIN_COUNT .so files already present, skipping build"
fi

# Step 7: Package
echo ""
echo "--- Creating release tarball ---"
cd "$CHROOT/home/we"

tar czf "$OUT" \
    --exclude='*.o' \
    --exclude='*.o.*' \
    --exclude='.git' \
    norns/build/matron/matron \
    norns/build/crone/crone \
    norns/build/maiden-repl/maiden-repl \
    norns/build/ws-wrapper/ws-wrapper \
    norns/build/watcher/watcher \
    norns/matronrc.lua \
    norns/resources/ \
    norns/lua/ \
    norns/sc/ \
    norns/doc/ \
    maiden/maiden \
    maiden/maiden.yaml \
    maiden/app/ \
    .local/share/SuperCollider/Extensions/ \
    2>/dev/null || true

SIZE=$(ls -lh "$OUT" | awk '{print $5}')
echo ""
echo "=== Package complete ==="
echo "Output: $OUT ($SIZE)"
echo ""
echo "Upload to GitHub Releases, then update NORNS_PREBUILT_URL in setup-norns.sh"

# Step 8: Create standalone SC plugins tarball
SC_PLUGINS_OUT="/data/UserData/sc-plugins-arm64.tar.gz"
echo ""
echo "--- Creating SC plugins tarball ---"
cd "$CHROOT/home/we"
if [ -d ".local/share/SuperCollider/Extensions" ]; then
    tar czf "$SC_PLUGINS_OUT" \
        -C .local/share/SuperCollider Extensions/
    SC_SIZE=$(ls -lh "$SC_PLUGINS_OUT" | awk '{print $5}')
    echo "Output: $SC_PLUGINS_OUT ($SC_SIZE)"
else
    echo "WARN: No SC Extensions directory found, skipping plugins tarball"
fi

echo "Also upload: $SC_PLUGINS_OUT"
echo "Then update SC_PLUGINS_URL in setup-norns.sh"

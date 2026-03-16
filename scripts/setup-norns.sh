#!/bin/sh
# setup-norns.sh — Install Norns into existing PipeWire chroot
# Run on Move: ssh root@move.local 'sh /data/setup-norns.sh'
#
# By default, downloads pre-built norns binaries from GitHub Releases.
# Set NORNS_BUILD_FROM_SOURCE=1 to clone and build from source instead.
set -e

CHROOT="/data/UserData/pw-chroot"
NORNS_HOME="$CHROOT/home/we"
BUILD_FROM_SOURCE="${NORNS_BUILD_FROM_SOURCE:-0}"

# Pre-built binary URL — update this when publishing a new release
PREBUILT_URL="${NORNS_PREBUILT_URL:-https://github.com/YOUR_USER/move-everything-norns/releases/latest/download/norns-move-prebuilt.tar.gz}"

if [ ! -d "$CHROOT/usr" ]; then
    echo "ERROR: Chroot not found at $CHROOT" >&2
    echo "Install the PipeWire module first." >&2
    exit 1
fi

echo "=== Installing Norns into chroot ==="
echo ""

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

echo "--- Installing apt packages ---"
chroot "$CHROOT" apt-get update
chroot "$CHROOT" apt-get install -y --no-install-recommends \
    supercollider-server supercollider-language sc3-plugins-server sc3-plugins-language \
    liblua5.3-0 libcairo2 liblo7 libevdev2 \
    libasound2t64 libsndfile1 libjack-jackd2-0 \
    libnanomsg5 libavahi-compat-libdnssd1 \
    libncursesw6 \
    lua5.3 \
    lua-lpeg lua-cjson lua-socket lua-filesystem \
    lua-posix lua-sec lua-lpeg-patterns \
    wget curl netcat-openbsd socat rsync \
    unzip zip sox libsox-fmt-all ffmpeg \
    file bc jq python3-pip git

# Only install dev packages if building from source
if [ "$BUILD_FROM_SOURCE" = "1" ]; then
    chroot "$CHROOT" apt-get install -y --no-install-recommends \
        liblua5.3-dev libcairo2-dev liblo-dev libevdev-dev \
        libasound2-dev libsndfile1-dev libjack-jackd2-dev \
        libnanomsg-dev libavahi-compat-libdnssd-dev libudev-dev \
        libglib2.0-dev \
        libncurses-dev libncursesw5-dev \
        gcc g++ git cmake python3 curl \
        golang
fi

echo "--- Setting up norns user home ---"
mkdir -p "$NORNS_HOME"
chown 1000:1000 "$NORNS_HOME"

if [ "$BUILD_FROM_SOURCE" = "1" ]; then
    echo "--- Cloning and building norns from source ---"
    chrt -o 0 chroot "$CHROOT" su - move -c '
        cd /home/we
        if [ ! -d norns ]; then
            git clone https://github.com/monome/norns.git
            cd norns
            git submodule update --init --recursive
        else
            cd norns
        fi
    '

    # Apply Move patches (FIFO I/O, no GPIO/SPI, etc.)
    if [ -f "$CHROOT/home/we/norns/patches/apply-move-patches.sh" ]; then
        chrt -o 0 chroot "$CHROOT" su - move -c \
            "cd /home/we/norns && sh patches/apply-move-patches.sh"
    elif [ -f /data/UserData/move-anything/modules/tools/norns/patches/apply-move-patches.sh ]; then
        cp /data/UserData/move-anything/modules/tools/norns/patches/apply-move-patches.sh \
            "$CHROOT/tmp/apply-move-patches.sh"
        chrt -o 0 chroot "$CHROOT" su - move -c \
            "cd /home/we/norns && sh /tmp/apply-move-patches.sh"
    else
        echo "WARNING: apply-move-patches.sh not found — norns will not build without it"
    fi

    chrt -o 0 chroot "$CHROOT" su - move -c '
        cd /home/we/norns
        python3 waf configure
        python3 waf build
    '

    echo "--- Cloning and building Maiden ---"
    chrt -o 0 chroot "$CHROOT" su - move -c '
        cd /home/we
        if [ ! -d maiden ]; then
            git clone https://github.com/monome/maiden.git
        fi
        cd maiden
        mkdir -p /home/we/go-cache
        GOCACHE=/home/we/go-cache GOTMPDIR=/home/we/go-cache go build -o maiden .
    '
    # Download pre-built Maiden web UI (React app — building requires Node.js/yarn)
    if [ ! -d "$CHROOT/home/we/maiden/app" ]; then
        echo "--- Downloading Maiden web UI ---"
        chroot "$CHROOT" su - move -c "
            cd /home/we
            curl -fsSL '$PREBUILT_URL' -o /tmp/maiden-ui.tar.gz
            tar xzf /tmp/maiden-ui.tar.gz maiden/app
            rm /tmp/maiden-ui.tar.gz
        "
    fi
else
    echo "--- Downloading pre-built norns binaries ---"
    if echo "$PREBUILT_URL" | grep -q "YOUR_USER"; then
        echo "ERROR: Set NORNS_PREBUILT_URL to the actual release URL" >&2
        echo "  or set NORNS_BUILD_FROM_SOURCE=1 to build from source" >&2
        exit 1
    fi

    # Run curl inside chroot where it's installed (host BusyBox lacks curl)
    chroot "$CHROOT" sh -c "
        cd /home/we
        curl -fsSL '$PREBUILT_URL' -o /tmp/norns-prebuilt.tar.gz
        tar xzf /tmp/norns-prebuilt.tar.gz
        rm -f /tmp/norns-prebuilt.tar.gz
        chown -R 1000:1000 /home/we/norns /home/we/maiden
    "
    echo "  Pre-built binaries installed"
fi

echo "--- Setting up dust directory ---"
chrt -o 0 chroot "$CHROOT" su - move -c '
    mkdir -p /home/we/dust/code /home/we/dust/audio /home/we/dust/data
'

echo "--- Installing starter scripts ---"
chrt -o 0 chroot "$CHROOT" su - move -c '
    cd /home/we/dust/code
    if which git >/dev/null 2>&1; then
        [ ! -d awake ] && git clone https://github.com/tehn/awake.git || true
        [ ! -d molly_the_poly ] && git clone https://github.com/markwheeler/molly_the_poly.git || true
        [ ! -d passersby ] && git clone https://github.com/markwheeler/passersby.git || true
    else
        echo "  git not installed — install scripts via Maiden"
    fi
'

echo "--- Creating sclang_conf.yaml ---"
# sclang needs explicit include paths to find norns core classes and
# user-installed engines in dust/code/. Without this, sclang only loads
# the system SC class library and no norns engines are available.
mkdir -p "$NORNS_HOME/.config/SuperCollider"
cat > "$NORNS_HOME/.config/SuperCollider/sclang_conf.yaml" << 'SCCONF'
includePaths:
    - /home/we/norns/sc/core
    - /home/we/norns/sc/engines
    - /home/we/dust
excludePaths:
    []
postInlinePaths: []
SCCONF
chown -R 1000:1000 "$NORNS_HOME/.config"

echo "--- Configuring scsynth for 48000 Hz ---"
mkdir -p "$NORNS_HOME/norns/sc"
cat > "$NORNS_HOME/norns/sc/startup.scd" << 'SCDEOF'
// Auto-generated for Move — force 48000 Hz sample rate
s.options.sampleRate = 48000;
s.options.protocol = \udp;
SCDEOF
chown 1000:1000 "$NORNS_HOME/norns/sc/startup.scd"

echo "--- Removing incompatible 32-bit SC plugins ---"
# norns ships PortedPlugins compiled for 32-bit ARM (Pi CM3).
# Move is 64-bit ARM — these crash scsynth with ELFCLASS32 errors.
rm -rf "$NORNS_HOME/.local/share/SuperCollider/Extensions/PortedPlugins" 2>/dev/null
echo "  Cleaned incompatible plugins"

echo "--- Ensuring /etc/hosts for localhost resolution ---"
# sclang's OSC library needs localhost to resolve for engine registration.
# A chroot may not have /etc/hosts if it wasn't created by debootstrap.
if [ ! -f "$CHROOT/etc/hosts" ] || ! grep -q '127.0.0.1' "$CHROOT/etc/hosts"; then
    cat > "$CHROOT/etc/hosts" << 'HOSTSEOF'
127.0.0.1	localhost
::1		localhost
HOSTSEOF
fi

echo "--- Configuring PipeWire ---"
# All PipeWire context.properties in ONE file — multiple files with the
# same section key can overwrite each other instead of merging.
mkdir -p "$CHROOT/etc/pipewire/pipewire.conf.d"
rm -f "$CHROOT/etc/pipewire/pipewire.conf.d/no-rt.conf"
cat > "$CHROOT/etc/pipewire/pipewire.conf.d/move-audio.conf" << 'PWEOF'
context.properties = {
    module.rt                   = false
    default.clock.rate          = 44100
    default.clock.allowed-rates = [ 44100 ]
    default.clock.quantum       = 1024
    default.clock.min-quantum   = 1024
    default.clock.max-quantum   = 1024
}
PWEOF

echo ""
echo "=== Norns Setup Complete ==="
echo "Load 'Norns' as a sound generator in Move Everything."
echo "Maiden: http://move.local:5000"

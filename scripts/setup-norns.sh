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
PREBUILT_URL="https://github.com/djhardrich/move-everything-norns/releases/download/v0.1.0/norns-move-prebuilt.tar.gz"

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
    supercollider-server supercollider-language sc3-plugins-server \
    liblua5.3-0 libcairo2 liblo7 libevdev2 \
    libasound2t64 libsndfile1 libjack-jackd2-0 \
    libnanomsg5 libavahi-compat-libdnssd1 \
    libncursesw6 \
    lua5.3 \
    lua-lpeg lua-cjson lua-socket lua-filesystem \
    lua-posix lua-sec lua-lpeg-patterns \
    wget curl netcat-openbsd socat rsync \
    unzip zip sox libsox-fmt-all ffmpeg \
    file bc jq python3-pip

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
    elif [ -f /data/UserData/move-anything/modules/sound_generators/norns/patches/apply-move-patches.sh ]; then
        cp /data/UserData/move-anything/modules/sound_generators/norns/patches/apply-move-patches.sh \
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
else
    echo "--- Downloading pre-built norns binaries ---"
    if echo "$PREBUILT_URL" | grep -q "YOUR_USER"; then
        echo "ERROR: Set NORNS_PREBUILT_URL to the actual release URL" >&2
        echo "  or set NORNS_BUILD_FROM_SOURCE=1 to build from source" >&2
        exit 1
    fi

    cd "$NORNS_HOME"
    curl -fsSL "$PREBUILT_URL" -o /tmp/norns-prebuilt.tar.gz
    tar xzf /tmp/norns-prebuilt.tar.gz
    rm -f /tmp/norns-prebuilt.tar.gz
    chown -R 1000:1000 "$NORNS_HOME/norns" "$NORNS_HOME/maiden"
    echo "  Pre-built binaries installed"
fi

echo "--- Setting up dust directory ---"
chrt -o 0 chroot "$CHROOT" su - move -c '
    mkdir -p /home/we/dust/code /home/we/dust/audio /home/we/dust/data
'

echo "--- Installing starter scripts ---"
chrt -o 0 chroot "$CHROOT" su - move -c '
    cd /home/we/dust/code
    if [ ! -d awake ]; then
        git clone https://github.com/monome/awake.git
    fi
    if [ ! -d molly_the_poly ]; then
        git clone https://github.com/markwheeler/molly_the_poly.git
    fi
    if [ ! -d passersby ]; then
        git clone https://github.com/markwheeler/passersby.git
    fi
'

echo "--- Configuring scsynth for 44100 Hz ---"
mkdir -p "$NORNS_HOME/norns/sc"
cat > "$NORNS_HOME/norns/sc/startup.scd" << 'SCDEOF'
// Auto-generated for Move — force 44100 Hz sample rate
s.options.sampleRate = 44100;
SCDEOF
chown 1000:1000 "$NORNS_HOME/norns/sc/startup.scd"

echo "--- Removing incompatible 32-bit SC plugins ---"
# norns ships PortedPlugins compiled for 32-bit ARM (Pi CM3).
# Move is 64-bit ARM — these crash scsynth with ELFCLASS32 errors.
rm -rf "$NORNS_HOME/.local/share/SuperCollider/Extensions/PortedPlugins" 2>/dev/null
echo "  Cleaned incompatible plugins"

echo "--- Configuring PipeWire no-RT ---"
mkdir -p "$CHROOT/etc/pipewire/pipewire.conf.d"
cat > "$CHROOT/etc/pipewire/pipewire.conf.d/no-rt.conf" << 'RTEOF'
context.properties = {
    module.rt = false
}
RTEOF

echo ""
echo "=== Norns Setup Complete ==="
echo "Load 'Norns' as a sound generator in Move Everything."
echo "Maiden: http://move.local:5000"

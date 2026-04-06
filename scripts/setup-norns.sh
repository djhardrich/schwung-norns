#!/bin/sh
# setup-norns.sh — Install Norns into existing PipeWire chroot
# Run on Move: ssh root@move.local 'sh /data/setup-norns.sh'
#
# By default, downloads pre-built norns binaries from GitHub Releases.
# Set NORNS_BUILD_FROM_SOURCE=1 to clone and build from source instead.
set -e

CHROOT="/data/UserData/pw-chroot"
NORNS_HOME="$CHROOT/home/we"
MODULE_DIR="/data/UserData/schwung/modules/tools/norns"
BUILD_FROM_SOURCE="${NORNS_BUILD_FROM_SOURCE:-0}"

# Pre-built binary URL — update this when publishing a new release
PREBUILT_URL="https://github.com/djhardrich/schwung-norns/releases/download/v0.4.0/norns-move-prebuilt.tar.gz"

# Pre-built 64-bit SC plugins URL — update this when publishing a new release
SC_PLUGINS_URL="https://github.com/djhardrich/schwung-norns/releases/download/v0.4.0/sc-plugins-arm64.tar.gz"
SC_PLUGINS_BUILD="${SC_PLUGINS_BUILD_FROM_SOURCE:-0}"

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
    git wget curl netcat-openbsd socat rsync \
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
        gcc g++ git cmake python3 python3-zombie-imp curl \
        golang \
        nodejs npm
fi

echo "--- Setting up norns user home ---"
mkdir -p "$NORNS_HOME"
chown 1000:1000 "$NORNS_HOME"

# Ensure move user's home is /home/we (norns expects $HOME/norns, $HOME/dust, etc.)
if chroot "$CHROOT" grep -q "^move:.*:/home/move:" /etc/passwd 2>/dev/null; then
    sed -i "s|move:x:1000:1000::/home/move:|move:x:1000:1000::/home/we:|" "$CHROOT/etc/passwd"
    echo "  Fixed move user home → /home/we"
fi

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

    # Reset patched files before (re-)applying patches
    chrt -o 0 chroot "$CHROOT" su - move -c '
        cd /home/we/norns
        git config --global --add safe.directory /home/we/norns
        git checkout -- wscript matron/wscript
    '

    # Apply Move patches (FIFO I/O, no GPIO/SPI, etc.)
    if [ -f "$MODULE_DIR/patches/apply-move-patches.sh" ]; then
        mkdir -p "$CHROOT/home/we/norns/patches"
        cp "$MODULE_DIR/patches/apply-move-patches.sh" "$CHROOT/home/we/norns/patches/"
        chrt -o 0 chroot "$CHROOT" su - move -c \
            "cd /home/we/norns && sh patches/apply-move-patches.sh"
    else
        echo "ERROR: apply-move-patches.sh not found at $MODULE_DIR/patches/" >&2
        exit 1
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

    echo "--- Building Maiden web UI ---"
    chroot "$CHROOT" npm install -g yarn
    chrt -o 0 chroot "$CHROOT" su - move -c '
        cd /home/we/maiden/web
        yarn install
        yarn build
        mkdir -p /home/we/maiden/app
        cp -r build /home/we/maiden/app/
    '
else
    echo "--- Downloading pre-built norns binaries ---"
    if echo "$PREBUILT_URL" | grep -q "YOUR_USER"; then
        echo "ERROR: Set NORNS_PREBUILT_URL to the actual release URL" >&2
        echo "  or set NORNS_BUILD_FROM_SOURCE=1 to build from source" >&2
        exit 1
    fi

    chrt -o 0 chroot "$CHROOT" su - move -c "
        cd /home/we
        curl -fsSL '$PREBUILT_URL' -o norns-prebuilt.tar.gz
        tar xzf norns-prebuilt.tar.gz
        rm -f norns-prebuilt.tar.gz
    "
    echo "  Pre-built binaries installed"
fi

# Ensure Move-specific matronrc.lua exists (FIFO drivers, not GPIO)
if [ ! -f "$CHROOT/home/we/norns/matronrc.lua" ]; then
    cat > "$CHROOT/home/we/norns/matronrc.lua" << 'MATRONRC'
-- matronrc.lua — Move-specific norns configuration
-- FIFO-based I/O: screen, input, and grid are handled by
-- the FIFO drivers compiled into matron (no GPIO/evdev needed).

function init_norns()
  _boot.add_io("keys:fifo", {})
  _boot.add_io("enc:fifo",  {index=1})
  _boot.add_io("enc:fifo",  {index=2})
  _boot.add_io("enc:fifo",  {index=3})
end

init_norns()
MATRONRC
    chown 1000:1000 "$CHROOT/home/we/norns/matronrc.lua"
    echo "  Created Move-specific matronrc.lua"
fi

echo "--- Setting up dust directory ---"
chrt -o 0 chroot "$CHROOT" su - move -c '
    mkdir -p /home/we/dust/code /home/we/dust/audio /home/we/dust/data
'

echo "--- Installing starter scripts ---"
chrt -o 0 chroot "$CHROOT" su - move -c '
    cd /home/we/dust/code
    if [ ! -d awake ]; then
        git clone https://github.com/tehn/awake.git
    fi
    if [ ! -d molly_the_poly ]; then
        git clone https://github.com/markwheeler/molly_the_poly.git
    fi
    if [ ! -d passersby ]; then
        git clone https://github.com/markwheeler/passersby.git
    fi
'

echo "--- Creating sclang_conf.yaml ---"
# sclang needs explicit include paths to find norns core classes and
# user-installed engines in dust/code/. Without this, sclang only loads
# the system SC class library and no norns engines are available.
cat > "$NORNS_HOME/norns/sclang_conf.yaml" << 'SCCONF'
includePaths:
    - /home/we/norns/sc/core
    - /home/we/norns/sc/engines
    - /home/we/norns/sc
    - /home/we/dust
excludePaths:
    []
postInlinePaths: []
SCCONF
chown 1000:1000 "$NORNS_HOME/norns/sclang_conf.yaml"

echo "--- Configuring scsynth for 44100 Hz ---"
mkdir -p "$NORNS_HOME/norns/sc"
cat > "$NORNS_HOME/norns/sc/startup.scd" << 'SCDEOF'
// Auto-generated for Move — force 44100 Hz sample rate
s.options.sampleRate = 44100;
SCDEOF
chown 1000:1000 "$NORNS_HOME/norns/sc/startup.scd"

echo "--- Installing 64-bit SC plugins ---"
# Replace 32-bit community plugins with native aarch64 builds.
# Default: download pre-built. Fallback: compile on-device.
SC_EXTENSIONS="$NORNS_HOME/.local/share/SuperCollider/Extensions"
mkdir -p "$SC_EXTENSIONS"

# Remove any existing 32-bit plugins first
chroot "$CHROOT" sh -c '
    for f in $(find /home/we/.local/share/SuperCollider/Extensions -name "*.so" 2>/dev/null); do
        case "$(file -b "$f")" in *32-bit*) rm -f "$f" ;; esac
    done
' 2>/dev/null

if [ "$SC_PLUGINS_BUILD" != "1" ]; then
    # Try downloading pre-built 64-bit plugins
    if chrt -o 0 chroot "$CHROOT" su - move -c "
        curl -fsSL '$SC_PLUGINS_URL' -o /tmp/sc-plugins-arm64.tar.gz
    " 2>/dev/null; then
        chrt -o 0 chroot "$CHROOT" su - move -c "
            tar xzf /tmp/sc-plugins-arm64.tar.gz -C /home/we/.local/share/SuperCollider/
            rm -f /tmp/sc-plugins-arm64.tar.gz
        "
        echo "  64-bit SC plugins installed from pre-built archive"
    else
        echo "  WARN: Pre-built download failed, falling back to on-device build"
        SC_PLUGINS_BUILD=1
    fi
fi

if [ "$SC_PLUGINS_BUILD" = "1" ]; then
    echo ""
    echo "  ============================================================"
    echo "  Building SuperCollider plugins from source on-device."
    echo "  This will take approximately 20-40 minutes."
    echo "  The Move may run warm during compilation."
    echo ""
    echo "  To use pre-built plugins instead, ensure network is available"
    echo "  and re-run without SC_PLUGINS_BUILD_FROM_SOURCE=1"
    echo "  ============================================================"
    echo ""

    # Install build deps inside chroot if needed
    chroot "$CHROOT" sh -c '
        if ! command -v cmake >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1; then
            apt-get update
            apt-get install -y --no-install-recommends gcc g++ cmake make git libsndfile1-dev
        fi
    '

    cp "$MODULE_DIR/scripts/build-sc-plugins.sh" "$CHROOT/tmp/"
    chrt -o 0 chroot "$CHROOT" su - move -c "sh /tmp/build-sc-plugins.sh"
    rm -f "$CHROOT/tmp/build-sc-plugins.sh"
    echo "  64-bit SC plugins built from source"
fi

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

# Dummy audio sink/source — creates system:playback_*/capture_* JACK ports.
# Move has no ALSA device inside the chroot, but crone requires system:playback
# ports to connect its output during startup.
cat > "$CHROOT/etc/pipewire/pipewire.conf.d/dummy-audio.conf" << 'DUMMYEOF'
context.objects = [
    {   factory = adapter
        args = {
            factory.name     = support.null-audio-sink
            node.name        = "system"
            media.class      = "Audio/Sink"
            object.linger    = true
            audio.position   = [ FL FR ]
            monitor.channel-volumes = true
        }
    }
    {   factory = adapter
        args = {
            factory.name     = support.null-audio-sink
            node.name        = "system"
            media.class      = "Audio/Source/Virtual"
            object.linger    = true
            audio.position   = [ FL FR ]
        }
    }
]
DUMMYEOF

echo ""
echo "=== Norns Setup Complete ==="
echo "Load 'Norns' as a tool in Schwung."
echo "Maiden: http://move.local:5000"

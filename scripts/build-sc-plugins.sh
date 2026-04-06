#!/bin/sh
# build-sc-plugins.sh — Compile SuperCollider UGen plugins for aarch64
# Run inside the Move's Debian chroot (as user 'move').
# Produces: ~/.local/share/SuperCollider/Extensions/<collection>/*.so
#
# Usage: sh build-sc-plugins.sh
# Env:   SC_PLUGINS_JOBS=3  (override parallel jobs, default 3)
set -e

EXTENSIONS="$HOME/.local/share/SuperCollider/Extensions"
BUILD_ROOT="/tmp/sc-plugin-build"
JOBS="${SC_PLUGINS_JOBS:-3}"

mkdir -p "$EXTENSIONS" "$BUILD_ROOT"

# --- Install build dependencies if missing ---
install_build_deps() {
    if ! command -v cmake >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1; then
        echo "--- Installing build dependencies ---"
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            gcc g++ cmake make git libsndfile1-dev
    fi
}

# --- Clone SuperCollider source headers (must match installed scsynth) ---
setup_sc_headers() {
    SC_VERSION=$(dpkg-query -W -f='${Version}' supercollider-server 2>/dev/null \
        | sed 's/^[0-9]*://' | sed 's/+.*//' | sed 's/-.*//' | sed 's/~.*//')
    if [ -z "$SC_VERSION" ]; then
        # Package not found — try scsynth binary version
        SC_VERSION=$(scsynth -v 2>&1 | grep -oP '\d+\.\d+\.\d+' | head -1 || echo "")
    fi
    if [ -z "$SC_VERSION" ]; then
        SC_VERSION="3.13.0"
        echo "  Could not detect SC version, using fallback: $SC_VERSION"
    fi
    echo "Installed SuperCollider version: $SC_VERSION"

    SC_SRC="$BUILD_ROOT/supercollider"
    if [ ! -d "$SC_SRC" ]; then
        echo "--- Cloning SuperCollider headers (Version-$SC_VERSION) ---"
        git clone --depth 1 --branch "Version-$SC_VERSION" \
            https://github.com/supercollider/supercollider.git "$SC_SRC" \
            2>/dev/null || {
            # Some versions use tag format without "Version-" prefix
            echo "  Tag 'Version-$SC_VERSION' not found, trying '$SC_VERSION'"
            git clone --depth 1 --branch "$SC_VERSION" \
                https://github.com/supercollider/supercollider.git "$SC_SRC" \
                2>/dev/null || {
                # Last resort: use latest release
                echo "  Tag '$SC_VERSION' not found either, using Version-3.13.0"
                git clone --depth 1 --branch "Version-3.13.0" \
                    https://github.com/supercollider/supercollider.git "$SC_SRC"
            }
        }
    fi
}

# --- Helper: build a standard cookiecutter SC plugin ---
# Args: $1=collection_name $2=repo_url $3=git_ref $4=extra_cmake_flags
build_plugin() {
    _name="$1"
    _repo="$2"
    _ref="$3"
    _extra_flags="$4"

    echo ""
    echo "=== Building $_name ==="
    _src="$BUILD_ROOT/$_name"

    if [ ! -d "$_src" ]; then
        git clone --depth 1 --branch "$_ref" --recursive "$_repo" "$_src" \
            2>/dev/null || git clone --depth 1 --recursive "$_repo" "$_src"
    fi

    mkdir -p "$_src/build"
    cd "$_src/build"

    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DSC_PATH="$SC_SRC" \
        -DSUPERNOVA=OFF \
        -DCMAKE_INSTALL_PREFIX="$EXTENSIONS/$_name" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        $_extra_flags

    # Try parallel build; fall back to single-threaded on OOM
    if ! cmake --build . --config Release -j"$JOBS" 2>&1; then
        echo "  WARN: Parallel build failed, retrying with -j1"
        cmake --build . --config Release -j1
    fi

    # Install if target exists, otherwise manual copy
    cmake --build . --target install 2>/dev/null || true

    # Verify .so files ended up in Extensions; if not, manual copy
    _installed=$(find "$EXTENSIONS/$_name" -name "*.so" 2>/dev/null | wc -l)
    if [ "$_installed" -gt 0 ]; then
        echo "  Installed $_name ($_installed .so files)"
    else
        echo "  Copying .so and .sc files manually"
        mkdir -p "$EXTENSIONS/$_name"
        find "$_src" -name "*_scsynth.so" -exec cp {} "$EXTENSIONS/$_name/" \; || true
        find "$_src" -name "*.so" -not -name "*_supernova.so" -exec cp {} "$EXTENSIONS/$_name/" \; || true
        find "$_src" -name "*.sc" -not -path "*/HelpSource/*" -exec cp {} "$EXTENSIONS/$_name/" \; || true
        _installed=$(find "$EXTENSIONS/$_name" -name "*.so" 2>/dev/null | wc -l)
        echo "  Copied $_installed .so files for $_name"
    fi

    echo "  Done: $_name"
    cd "$BUILD_ROOT"
}

install_build_deps
setup_sc_headers

echo ""
echo "=== Building all SC plugin collections ==="
echo "  Jobs: $JOBS | SC headers: $SC_SRC"
echo ""

# --- Standard cookiecutter plugins (have install targets) ---

# PortedPlugins: DaisySP's dsp.h has #ifdef __arm__ guards for 32-bit VFP
# intrinsics. On aarch64, __arm__ is undefined so the portable C++ path
# is used automatically. If compilation fails, apply the sed patch.
build_plugin "PortedPlugins" \
    "https://github.com/madskjeldgaard/portedplugins.git" \
    "main" \
    ""
# If PortedPlugins failed due to __arm__ issues, patch and retry:
if [ ! -f "$EXTENSIONS/PortedPlugins/"*_scsynth.so ] 2>/dev/null; then
    echo "  WARN: PortedPlugins may have failed, applying DaisySP ARM patch and retrying"
    _pp_src="$BUILD_ROOT/PortedPlugins"
    find "$_pp_src" -name "dsp.h" -exec sed -i 's/__arm__/__armdisable__/g' {} \;
    rm -rf "$_pp_src/build"
    build_plugin "PortedPlugins" \
        "https://github.com/madskjeldgaard/portedplugins.git" \
        "main" \
        ""
fi

build_plugin "f0plugins" \
    "https://github.com/redFrik/f0plugins.git" \
    "master" \
    ""

build_plugin "XPlayBuf" \
    "https://github.com/elgiano/XPlayBuf.git" \
    "master" \
    ""

build_plugin "NasalDemons" \
    "https://github.com/elgiano/NasalDemons.git" \
    "main" \
    ""

build_plugin "PulsePTR" \
    "https://github.com/robbielyman/pulseptr.git" \
    "main" \
    ""

build_plugin "TrianglePTR" \
    "https://github.com/robbielyman/triangleptr.git" \
    "main" \
    ""

build_plugin "CDSkip" \
    "https://github.com/nhthn/supercollider-cd-skip.git" \
    "main" \
    ""

# --- mi-UGens (Mutable Instruments ports) ---
# SUPERNOVA=OFF is already set by build_plugin helper; no extra flags needed

build_plugin "mi-UGens" \
    "https://github.com/v7b1/mi-UGens.git" \
    "master" \
    ""

# --- SuperBuf (no install target) ---

build_plugin "SuperBuf" \
    "https://github.com/esluyter/super-bufrd.git" \
    "master" \
    ""

# --- IBufWr (no install target, requires C++20) ---

build_plugin "IBufWr" \
    "https://github.com/tremblap/IBufWr.git" \
    "main" \
    "-DCMAKE_CXX_STANDARD=20"

# --- Cleanup build artifacts and dependencies ---
echo ""
echo "=== Cleaning up ==="
rm -rf "$BUILD_ROOT"

# Remove build dependencies to reclaim ~200MB on the 2GB device
echo "  Removing build dependencies..."
sudo apt-get remove --purge -y gcc g++ cmake make libsndfile1-dev 2>/dev/null || true
sudo apt-get autoremove -y 2>/dev/null || true

# Report results
echo ""
echo "=== Build complete ==="
TOTAL=$(find "$EXTENSIONS" -name "*_scsynth.so" -o -name "*.so" 2>/dev/null | wc -l)
echo "Installed $TOTAL .so files to $EXTENSIONS"
echo ""
ls -d "$EXTENSIONS"/*/ 2>/dev/null | while read -r d; do
    _count=$(find "$d" -name "*.so" 2>/dev/null | wc -l)
    echo "  $(basename "$d"): $_count .so files"
done

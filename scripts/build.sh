#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-norns-builder"
OUTPUT_BASENAME="${OUTPUT_BASENAME:-norns-module}"

# ── If running outside Docker, re-exec inside container ──
if [ ! -f /.dockerenv ]; then
    echo "=== Building Docker image ==="
    docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"

    rm -rf "$REPO_ROOT/build" "$REPO_ROOT/dist"

    echo "=== Running build inside container ==="
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        -e OUTPUT_BASENAME="$OUTPUT_BASENAME" \
        "$IMAGE_NAME" ./scripts/build.sh
    exit $?
fi

# ── Inside Docker: cross-compile ──
echo "=== Cross-compiling dsp.so ==="
mkdir -p build/module

"${CROSS_PREFIX}gcc" -O3 -g -shared -fPIC \
    src/dsp/norns_plugin.c \
    -o build/module/dsp.so \
    -Isrc/dsp \
    -lpthread -lm

echo "=== Cross-compiling pw-helper ==="
"${CROSS_PREFIX}gcc" -O2 -static \
    src/pw-helper.c \
    -o build/pw-helper

echo "=== Cross-compiling norns-input-bridge ==="
"${CROSS_PREFIX}gcc" -O2 -Wall \
    src/norns-input-bridge.c \
    -o build/norns-input-bridge

echo "=== Cross-compiling jack-fifo-bridge ==="
"${CROSS_PREFIX}gcc" -O2 -Wall \
    src/jack-fifo-bridge.c \
    -o build/jack-fifo-bridge \
    $(PKG_CONFIG_PATH=${PKG_CONFIG_PATH} pkg-config --cflags --libs jack 2>/dev/null || echo "-ljack")

echo "=== Assembling module package ==="
cp src/module.json  build/module/
cp src/ui.js        build/module/
cp src/start-norns.sh    build/module/
cp src/stop-norns.sh     build/module/
cp src/restart-norns.sh  build/module/
chmod +x build/module/start-norns.sh build/module/stop-norns.sh build/module/restart-norns.sh

mkdir -p build/module/bin
cat build/pw-helper > build/module/bin/pw-helper
chmod +x build/module/bin/pw-helper
cat build/norns-input-bridge > build/module/bin/norns-input-bridge
chmod +x build/module/bin/norns-input-bridge
cat build/jack-fifo-bridge > build/module/bin/jack-fifo-bridge
chmod +x build/module/bin/jack-fifo-bridge

# ── Package ──
rm -rf dist
mkdir -p dist
tar -cf - -C build module | tar -xf - -C dist && mv dist/module dist/norns

(cd dist && tar -czvf "${OUTPUT_BASENAME}.tar.gz" norns/)

echo ""
echo "=== Build complete ==="
echo "Module: dist/${OUTPUT_BASENAME}.tar.gz"
echo "Files:  dist/norns/"

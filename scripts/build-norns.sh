#!/usr/bin/env bash
# build-norns.sh — Build norns prebuilt tarball in Docker (Apple Silicon native arm64)
#
# Produces: dist/norns-move-prebuilt.tar.gz
# This replaces the slow on-device build. The tarball is downloaded by
# setup-norns.sh when users run the default (non-source) install path.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="norns-move-builder"
OUTPUT="$REPO_ROOT/dist/norns-move-prebuilt.tar.gz"

echo "=== Building norns prebuilt tarball in Docker ==="
echo ""

# Check for Apple Silicon (native arm64 — no QEMU needed)
ARCH=$(uname -m)
if [ "$ARCH" != "arm64" ] && [ "$ARCH" != "aarch64" ]; then
    echo "WARNING: Not running on arm64 ($ARCH). Docker will use QEMU emulation (slow)."
    echo "  This build is optimized for Apple Silicon where arm64 runs natively."
    echo ""
fi

# Build the Docker image
echo "--- Building Docker image ---"
docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile.norns" "$REPO_ROOT"

# Run the build inside Docker
echo ""
echo "--- Building norns inside container ---"
mkdir -p "$REPO_ROOT/dist"

docker run --rm \
    -v "$REPO_ROOT/patches:/patches:ro" \
    -v "$REPO_ROOT/dist:/output" \
    "$IMAGE_NAME" bash -c '
set -e

echo "=== Cloning norns ==="
cd /home/we
git clone --depth 1 https://github.com/monome/norns.git
cd norns
git submodule update --init --recursive

echo ""
echo "=== Applying Move patches ==="
sh /patches/apply-move-patches.sh /home/we/norns

echo ""
echo "=== Building norns (matron + crone) ==="
python3 waf configure
python3 waf build

echo ""
echo "=== Cloning and building Maiden ==="
cd /home/we
git clone --depth 1 https://github.com/monome/maiden.git
cd maiden
mkdir -p /home/we/go-cache
GOCACHE=/home/we/go-cache GOTMPDIR=/home/we/go-cache go build -o maiden .

echo ""
echo "=== Building Maiden web UI ==="
cd /home/we/maiden/web
yarn install
yarn build
mkdir -p /home/we/maiden/app
cp -r build /home/we/maiden/app/

echo ""
echo "=== Packaging prebuilt tarball ==="
cd /home/we
tar czf /output/norns-move-prebuilt.tar.gz \
    --exclude="*.o" \
    --exclude="*.o.*" \
    --exclude=".git" \
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
    2>/dev/null || true

echo ""
echo "=== Build complete ==="
ls -lh /output/norns-move-prebuilt.tar.gz
'

echo ""
echo "=== Norns prebuilt tarball ready ==="
echo "Output: $OUTPUT"
ls -lh "$OUTPUT"
echo ""
echo "Upload to GitHub Releases, then update PREBUILT_URL in scripts/setup-norns.sh"

#!/usr/bin/env bash
# Build Dexed module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Dexed Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Dexed Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/dexed

# Compile DSP plugin
echo "Compiling DSP plugin..."
${CROSS_PREFIX}g++ -g -O3 -shared -fPIC -std=c++14 \
    src/dsp/dx7_plugin.cpp \
    src/dsp/msfa/dx7note.cc \
    src/dsp/msfa/env.cc \
    src/dsp/msfa/exp2.cc \
    src/dsp/msfa/fm_core.cc \
    src/dsp/msfa/fm_op_kernel.cc \
    src/dsp/msfa/freqlut.cc \
    src/dsp/msfa/lfo.cc \
    src/dsp/msfa/pitchenv.cc \
    src/dsp/msfa/sin.cc \
    src/dsp/msfa/porta.cpp \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > dist/dexed/module.json
cat src/ui.js > dist/dexed/ui.js
cat build/dsp.so > dist/dexed/dsp.so
chmod +x dist/dexed/dsp.so

# Create tarball for release
cd dist
tar -czvf dexed-module.tar.gz dexed/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/dexed/"
echo "Tarball: dist/dexed-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"

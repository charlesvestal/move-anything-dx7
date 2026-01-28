#!/bin/bash
# Install Dexed module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/dexed" ]; then
    echo "Error: dist/dexed not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Dexed Module ==="

# Deploy to Move - sound_generators subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/sound_generators/dexed"
scp -r dist/dexed/* ableton@move.local:/data/UserData/move-anything/modules/sound_generators/dexed/

# Copy banks if included in dist, otherwise create empty directory for user .syx files
if [ -d "dist/dexed/banks" ] && [ "$(ls -A dist/dexed/banks/*.syx 2>/dev/null)" ]; then
    echo "Installing patch banks..."
    ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/sound_generators/dexed/banks"
    scp dist/dexed/banks/*.syx ableton@move.local:/data/UserData/move-anything/modules/sound_generators/dexed/banks/
else
    echo "Creating banks directory..."
    ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/sound_generators/dexed/banks"
fi

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/patches/
fi

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/sound_generators/dexed"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/sound_generators/dexed/"
echo ""
echo "Restart Move Anything to load the new module."

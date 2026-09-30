#!/bin/sh
# Install Kraken Explorer from source.
# Usage: scripts/install.sh [build-dir]   (default: build/install)
# Installs build dependencies, builds a .deb and installs it.
set -eu

DEPS="cmake ninja-build gcc g++ libgl-dev libusb-1.0-0-dev \
      libnl-3-dev libnl-route-3-dev python3-dev pybind11-dev \
      pkg-config dpkg-dev"

cd "$(dirname "$0")/.."

echo "==> Installing build dependencies..."
sudo apt-get install -y $DEPS

echo "==> Building .deb..."
BUILD=${1:-build/install}
scripts/build_deb.sh "$BUILD"

DEB=$(ls "$BUILD"/*.deb | head -1)
echo "==> Installing $DEB..."
sudo dpkg -i "$DEB"

echo ""
echo "Done. Run: kraken-explorer"

#!/bin/bash
#
# setup_pi.sh — One-time setup for Raspberry Pi 5 (64-bit Raspberry Pi OS Bookworm).
# Installs all dependencies, then builds the KVS WebRTC SDK and GStreamer samples.
#
# Usage:
#   chmod +x setup_pi.sh
#   ./setup_pi.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=========================================="
echo " KVS WebRTC SDK — Raspberry Pi 5 Setup"
echo "=========================================="
echo ""
echo "Project dir : $SCRIPT_DIR"
echo "Build dir   : $BUILD_DIR"
echo ""

# ── 1. Install system packages ────────────────────────────────────────────────

echo "[1/3] Installing system packages..."
sudo apt-get update
sudo apt-get install -y \
    cmake m4 pkg-config gcc g++ make \
    libssl-dev libcurl4-openssl-dev liblog4cplus-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base-apps gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly \
    gstreamer1.0-tools gstreamer1.0-x gstreamer1.0-libav
echo ""
echo "  System packages installed."
echo ""

# ── 2. Configure with CMake ──────────────────────────────────────────────────

echo "[2/3] Configuring build (cmake)..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" \
    -DBUILD_OPENSSL_PLATFORM=linux-aarch64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DIOT_CORE_ENABLE_CREDENTIALS=ON

echo ""
echo "  CMake configuration complete."
echo ""

# ── 3. Build ─────────────────────────────────────────────────────────────────

echo "[3/3] Building (this may take a while on the Pi)..."
# Use all available cores but cap at 4 to avoid running out of memory
NCORES=$(nproc 2>/dev/null || echo 4)
if [ "$NCORES" -gt 4 ]; then NCORES=4; fi

make -j"$NCORES"

echo ""
echo "=========================================="
echo " Build complete!"
echo "=========================================="
echo ""
echo "The sample binary is at:"
echo "  $BUILD_DIR/samples/kvsWebrtcClientMasterGstSample"
echo ""
echo "Next steps:"
echo "  1. Ensure your X.509 certs are in ~/certs/ (*.private.key, *.cert.pem, AmazonRootCA1.pem)"
echo "  2. Run:  ./run_3_streams_pi.sh"
echo ""
echo "The IoT Thing name is derived automatically from the hostname:"
echo "  $(hostname) → pi_$(echo "$(hostname)" | sed 's/^actionbricks-//' | tr '-' '_')"
echo ""

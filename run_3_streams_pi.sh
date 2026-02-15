#!/bin/bash
#
# run_3_streams_pi.sh — Launch 3 camera streams on Raspberry Pi 5.
#
# Each USB camera streams to its own KVS signaling channel:
#   USB Camera 1 → actionbricks_demo_darts_camera_1
#   USB Camera 2 → actionbricks_demo_darts_camera_2
#   USB Camera 3 → actionbricks_demo_darts_camera_3
#
# Prerequisites:
#   - Built with ./setup_pi.sh
#   - AWS credentials exported in your environment:
#       export AWS_ACCESS_KEY_ID="..."
#       export AWS_SECRET_ACCESS_KEY="..."
#       export AWS_SESSION_TOKEN="..."
#       export AWS_DEFAULT_REGION="us-east-1"
#
# Usage:
#   ./run_3_streams_pi.sh
#
# Press Ctrl+C to stop all streams.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BINARY="$BUILD_DIR/samples/kvsWebrtcClientMasterGstSample"

# ── Verify binary exists ─────────────────────────────────────────────────────

if [ ! -x "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY"
    echo "Run ./setup_pi.sh first to build the project."
    exit 1
fi

# ── Verify AWS credentials ───────────────────────────────────────────────────

if [ -z "${AWS_ACCESS_KEY_ID:-}" ] || [ -z "${AWS_SECRET_ACCESS_KEY:-}" ]; then
    echo "ERROR: AWS credentials not set."
    echo ""
    echo "Export them before running this script:"
    echo "  export AWS_ACCESS_KEY_ID=\"...\""
    echo "  export AWS_SECRET_ACCESS_KEY=\"...\""
    echo "  export AWS_SESSION_TOKEN=\"...\"   # if using temporary credentials"
    echo "  export AWS_DEFAULT_REGION=\"us-east-1\""
    exit 1
fi

# ── Pi-optimized video pipeline settings ─────────────────────────────────────
# These can be overridden by exporting the variables before running this script.
# Defaults are tuned for 3 simultaneous streams on a Raspberry Pi 5.

export KVS_VIDEO_WIDTH="${KVS_VIDEO_WIDTH:-640}"
export KVS_VIDEO_HEIGHT="${KVS_VIDEO_HEIGHT:-480}"
export KVS_VIDEO_FPS="${KVS_VIDEO_FPS:-20}"
export KVS_VIDEO_BITRATE="${KVS_VIDEO_BITRATE:-384}"
export KVS_ENCODER_PRESET="${KVS_ENCODER_PRESET:-ultrafast}"

# ── Trap Ctrl+C to stop all streams ──────────────────────────────────────────

cleanup() {
    echo ""
    echo "Stopping all streams..."
    kill $(jobs -p) 2>/dev/null
    wait 2>/dev/null
    echo "All streams stopped."
    exit 0
}
trap cleanup SIGINT SIGTERM

# ── Launch ────────────────────────────────────────────────────────────────────

echo "=========================================="
echo " Starting 3 KVS WebRTC Camera Streams"
echo "       (Raspberry Pi 5 mode)"
echo "=========================================="
echo ""
echo "  Video : ${KVS_VIDEO_WIDTH}x${KVS_VIDEO_HEIGHT} @ ${KVS_VIDEO_FPS} fps"
echo "  Encode: x264enc preset=${KVS_ENCODER_PRESET}, bitrate=${KVS_VIDEO_BITRATE} kbps"
echo ""
echo "  Camera 0 (device index 0) → actionbricks_demo_darts_camera_1"
echo "  Camera 1 (device index 1) → actionbricks_demo_darts_camera_2"
echo "  Camera 2 (device index 2) → actionbricks_demo_darts_camera_3"
echo ""
echo "Press Ctrl+C to stop all streams"
echo "=========================================="
echo ""

cd "$BUILD_DIR"

# Launch all 3 streams in the background
./samples/kvsWebrtcClientMasterGstSample actionbricks_demo_darts_camera_1 video-only devicesrc 0 &
PID1=$!

./samples/kvsWebrtcClientMasterGstSample actionbricks_demo_darts_camera_2 video-only devicesrc 1 &
PID2=$!

./samples/kvsWebrtcClientMasterGstSample actionbricks_demo_darts_camera_3 video-only devicesrc 2 &
PID3=$!

echo "Stream PIDs: CAM1=$PID1  CAM2=$PID2  CAM3=$PID3"
echo ""

# Wait forever until Ctrl+C
wait

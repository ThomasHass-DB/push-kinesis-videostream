#!/bin/bash
#
# run_3_streams_pi.sh — Launch 3 camera streams on Raspberry Pi 5.
#
# Each USB camera streams to its own KVS signaling channel:
#   USB Camera 1 → actionbricks_demo_darts_camera_1
#   USB Camera 2 → actionbricks_demo_darts_camera_2
#   USB Camera 3 → actionbricks_demo_darts_camera_3
#
# Authentication uses AWS IoT Core X.509 certificates, which automatically
# refresh temporary credentials — no manual key rotation needed.
#
# The IoT Thing name is derived from the Pi's hostname:
#   actionbricks-germany-01  →  pi_germany_01
#
# Prerequisites:
#   - Built with ./setup_pi.sh (includes -DIOT_CORE_ENABLE_CREDENTIALS=ON)
#   - Certificates installed in ~/certs/:
#       <name>.private.key   — IoT device private key
#       <name>.cert.pem      — IoT device X.509 certificate
#       AmazonRootCA1.pem    — Amazon root CA
#   - (Optional) ~/certs/iot.env to override defaults — see streaming/iot.env.template
#
# Usage:
#   ./run_3_streams_pi.sh
#
# Press Ctrl+C to stop all streams.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BINARY="$BUILD_DIR/samples/kvsWebrtcClientMasterGstSample"
CERTS_DIR="${IOT_CERTS_DIR:-$HOME/certs}"
IOT_ENV_FILE="$CERTS_DIR/iot.env"

# ── Load overrides from iot.env (if present) ─────────────────────────────────
# Any variable set in iot.env takes precedence over the defaults below.

if [ -f "$IOT_ENV_FILE" ]; then
    set -a
    source "$IOT_ENV_FILE"
    set +a
fi

# ── IoT Core configuration (defaults, overridable via iot.env) ───────────────

IOT_CREDENTIAL_ENDPOINT="${AWS_IOT_CORE_CREDENTIAL_ENDPOINT:-c3h0j7jh565u8z.credentials.iot.us-west-2.amazonaws.com}"
IOT_ROLE_ALIAS="${AWS_IOT_CORE_ROLE_ALIAS:-actionbricks_pi_alias}"
IOT_REGION="${AWS_DEFAULT_REGION:-us-west-2}"

# ── Derive IoT Thing name from hostname (unless overridden) ──────────────────
# actionbricks-germany-01 → pi_germany_01

RAW_HOSTNAME="$(hostname)"
IOT_THING_NAME="${AWS_IOT_CORE_THING_NAME:-pi_$(echo "$RAW_HOSTNAME" | sed 's/^actionbricks-//' | tr '-' '_')}"

# ── Verify binary exists ─────────────────────────────────────────────────────

if [ ! -x "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY"
    echo "Run ./setup_pi.sh first to build the project."
    exit 1
fi

# ── Auto-discover certificates ───────────────────────────────────────────────

CA_CERT="$CERTS_DIR/AmazonRootCA1.pem"

if [ ! -f "$CA_CERT" ]; then
    echo "ERROR: Amazon root CA not found at $CA_CERT"
    exit 1
fi

if [ -z "${AWS_IOT_CORE_CERT:-}" ]; then
    IOT_CERT_FILE=$(find "$CERTS_DIR" -maxdepth 1 -name '*.cert.pem' -type f | head -n 1)
    if [ -z "$IOT_CERT_FILE" ]; then
        echo "ERROR: No *.cert.pem found in $CERTS_DIR"
        exit 1
    fi
    export AWS_IOT_CORE_CERT="$IOT_CERT_FILE"
fi

if [ -z "${AWS_IOT_CORE_PRIVATE_KEY:-}" ]; then
    IOT_KEY_FILE=$(find "$CERTS_DIR" -maxdepth 1 -name '*.private.key' -type f | head -n 1)
    if [ -z "$IOT_KEY_FILE" ]; then
        echo "ERROR: No *.private.key found in $CERTS_DIR"
        exit 1
    fi
    export AWS_IOT_CORE_PRIVATE_KEY="$IOT_KEY_FILE"
fi

if [ ! -f "$AWS_IOT_CORE_CERT" ]; then
    echo "ERROR: IoT certificate not found: $AWS_IOT_CORE_CERT"
    exit 1
fi
if [ ! -f "$AWS_IOT_CORE_PRIVATE_KEY" ]; then
    echo "ERROR: IoT private key not found: $AWS_IOT_CORE_PRIVATE_KEY"
    exit 1
fi

# ── Export IoT Core env vars for the KVS SDK ─────────────────────────────────

export AWS_IOT_CORE_CREDENTIAL_ENDPOINT="$IOT_CREDENTIAL_ENDPOINT"
export AWS_IOT_CORE_ROLE_ALIAS="$IOT_ROLE_ALIAS"
export AWS_IOT_CORE_THING_NAME="$IOT_THING_NAME"
export AWS_KVS_CACERT_PATH="$CA_CERT"
export AWS_DEFAULT_REGION="$IOT_REGION"

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
echo "  (Raspberry Pi 5 — IoT Core Auth)"
echo "=========================================="
echo ""
echo "  Hostname: $RAW_HOSTNAME"
echo "  Thing   : $AWS_IOT_CORE_THING_NAME"
echo "  Endpoint: $AWS_IOT_CORE_CREDENTIAL_ENDPOINT"
echo "  Alias   : $AWS_IOT_CORE_ROLE_ALIAS"
echo "  Cert    : $AWS_IOT_CORE_CERT"
echo "  Key     : $AWS_IOT_CORE_PRIVATE_KEY"
echo "  CA      : $CA_CERT"
echo "  Region  : $AWS_DEFAULT_REGION"
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

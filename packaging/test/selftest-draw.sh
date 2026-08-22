#!/usr/bin/env bash
#
# Integration test for the software-frame H.264 path, run against the installed
# library rather than the build tree.
#
# examples/draw is a neatvnc server that feeds frame buffers from main memory,
# exactly like Weston's VNC backend does, so this exercises encoder selection,
# the software-frame gate in choose_frame_encoding(), the AVFrame wrapping and
# the RFB framing. NEATVNC_H264_NVENC_CODEC stands a software encoder in for
# NVENC, because build machines have no GPU; everything else is the real path.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${VNC_PORT:-5900}"
LOG=/tmp/nvnc-draw.log

NVNC_LOG_LEVEL=debug \
NEATVNC_H264_ENCODER=nvenc \
NEATVNC_H264_NVENC_CODEC="${NEATVNC_H264_NVENC_CODEC:-libx264}" \
	nvnc-draw > "${LOG}" 2>&1 &
SERVER_PID=$!

cleanup() {
	kill "${SERVER_PID}" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 100); do
	if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
		echo "nvnc-draw exited during startup:" >&2
		cat "${LOG}" >&2
		exit 1
	fi
	if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
		break
	fi
	sleep 0.2
done

rc=0
python3 "${SCRIPT_DIR}/rfb-h264-client.py" --port "${PORT}" || rc=$?

echo "=== server log ==="
cat "${LOG}"

exit "${rc}"

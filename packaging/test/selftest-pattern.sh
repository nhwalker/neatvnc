#!/usr/bin/env bash
#
# Pixel-level test for the software-frame H.264 path.
#
# h264-test-server.c serves a known pattern from a frame buffer in main memory
# with a deliberately padded stride; rfb-h264-client.py collects several frames,
# decodes them with ffmpeg and compares the result against that pattern. A
# well-formed bitstream is not enough here: a stride mix-up or a swapped colour
# channel produces perfectly valid H.264 of the wrong image, and only the
# decode-and-compare catches it.
#
# It also collects more than one frame, so P-frames are exercised rather than
# just the first keyframe.
#
# Works against either the installed library or a meson build tree; set
# NEATVNC_BUILD_DIR to use the latter. NEATVNC_H264_NVENC_CODEC stands a
# software encoder in for NVENC, because test machines have no GPU.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${VNC_PORT:-5901}"
FRAMES="${FRAMES:-8}"
LOG=/tmp/h264-test-server.log

WORK="$(mktemp -d)"
SERVER_BIN="${WORK}/h264-test-server"

trap 'rm -rf "${WORK}"' EXIT

if [ -n "${NEATVNC_BUILD_DIR:-}" ]; then
	REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
	BUILD="$(cd "${NEATVNC_BUILD_DIR}" && pwd)"
	echo "Building the test server against ${BUILD}"
	gcc -O2 -o "${SERVER_BIN}" "${SCRIPT_DIR}/h264-test-server.c" \
		-DAML_UNSTABLE_API=1 \
		-I"${REPO_DIR}/include" \
		-I"${REPO_DIR}/subprojects/aml/include" \
		$(pkg-config --cflags pixman-1) \
		-L"${BUILD}" -lneatvnc \
		-L"${BUILD}/subprojects/aml" -laml \
		$(pkg-config --libs pixman-1) -lm
	export LD_LIBRARY_PATH="${BUILD}:${BUILD}/subprojects/aml${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
else
	echo "Building the test server against the installed library"
	gcc -O2 -o "${SERVER_BIN}" "${SCRIPT_DIR}/h264-test-server.c" \
		-DAML_UNSTABLE_API=1 \
		$(pkg-config --cflags --libs neatvnc pixman-1 aml) -lm
fi

NVNC_LOG_LEVEL=debug \
NEATVNC_H264_ENCODER=nvenc \
NEATVNC_H264_NVENC_CODEC="${NEATVNC_H264_NVENC_CODEC:-libx264}" \
	"${SERVER_BIN}" 127.0.0.1 "${PORT}" > "${LOG}" 2>&1 &
SERVER_PID=$!

cleanup() {
	kill "${SERVER_PID}" 2>/dev/null || true
	rm -rf "${WORK}"
}
trap cleanup EXIT

for _ in $(seq 1 100); do
	if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
		echo "test server exited during startup:" >&2
		cat "${LOG}" >&2
		exit 1
	fi
	if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
		break
	fi
	sleep 0.2
done

# Something else may already own the port -- notably when the container shares
# the host's network namespace -- in which case the connect above succeeds
# against the wrong server.
if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
	echo "the server is not running; is port ${PORT} already taken?" >&2
	cat "${LOG}" >&2
	exit 1
fi

rc=0
python3 "${SCRIPT_DIR}/rfb-h264-client.py" \
	--port "${PORT}" --frames "${FRAMES}" --verify-pattern || rc=$?

echo "=== server log ==="
cat "${LOG}"

exit "${rc}"

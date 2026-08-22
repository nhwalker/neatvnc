#!/usr/bin/env bash
#
# Starts Weston's VNC backend, optionally with websockify and noVNC in front.
#
#   entrypoint serve      Weston + websockify + noVNC, stays in the foreground
#   entrypoint selftest   checks that the stack comes up, then exits
#   entrypoint <cmd>...   runs <cmd> instead

set -euo pipefail

MODE="${1:-serve}"

mkdir -p "${XDG_RUNTIME_DIR:=/run/user/0}"
chmod 700 "${XDG_RUNTIME_DIR}"
export XDG_RUNTIME_DIR

WESTON_LOG=/tmp/weston.log
CERT=/tmp/novnc.pem

start_weston() {
	echo "Starting weston on :${VNC_PORT} (${WESTON_WIDTH}x${WESTON_HEIGHT})"

	# TLS is disabled on the VNC socket because websockify terminates TLS in
	# front of it and the loopback hop never leaves the container. Weston
	# still requires a PAM login; see set_password below.
	weston \
		--backend=vnc \
		--shell=kiosk \
		--address=127.0.0.1 \
		--port="${VNC_PORT}" \
		--width="${WESTON_WIDTH}" \
		--height="${WESTON_HEIGHT}" \
		--disable-transport-layer-security \
		--log="${WESTON_LOG}" &
	WESTON_PID=$!

	for _ in $(seq 1 100); do
		if ! kill -0 "${WESTON_PID}" 2>/dev/null; then
			echo "weston exited during startup:" >&2
			cat "${WESTON_LOG}" >&2 || true
			return 1
		fi
		if (exec 3<>"/dev/tcp/127.0.0.1/${VNC_PORT}") 2>/dev/null; then
			echo "weston is listening"
			return 0
		fi
		sleep 0.2
	done

	echo "weston did not start listening on ${VNC_PORT}" >&2
	cat "${WESTON_LOG}" >&2 || true
	return 1
}

set_password() {
	# Weston's vnc_handle_auth() rejects any username other than the one it
	# runs as, so the VNC login is whoever owns this process.
	local user
	user="$(id -un)"

	if [ -z "${VNC_PASSWORD:-}" ]; then
		VNC_PASSWORD="$(openssl rand -base64 12)"
		echo "VNC_PASSWORD was not set; generated one for this run."
	fi

	echo "${user}:${VNC_PASSWORD}" | chpasswd
	echo "Log in as '${user}' with password '${VNC_PASSWORD}'"
}

make_cert() {
	if [ ! -f "${CERT}" ]; then
		openssl req -new -x509 -days 365 -nodes \
			-subj "/CN=localhost" \
			-out "${CERT}" -keyout "${CERT}" > /dev/null 2>&1
	fi
}

case "${MODE}" in
serve)
	set_password
	start_weston
	make_cert

	echo "noVNC on https://localhost:${WEB_PORT}/vnc.html"
	exec websockify --web /usr/share/novnc --cert "${CERT}" \
		"0.0.0.0:${WEB_PORT}" "127.0.0.1:${VNC_PORT}"
	;;
selftest)
	rc=0

	# The library under test must be the patched one, not EPEL's.
	rpm -q --qf '%{name}-%{version}-%{release}\n' neatvnc
	rpm -q neatvnc | grep -q nvenc || rc=1

	# noVNC must be new enough to have an H.264 decoder at all.
	test -f /usr/share/novnc/core/decoders/h264.js || rc=1
	echo "noVNC h264 decoder: present"

	set_password
	start_weston || rc=1

	make_cert
	websockify --web /usr/share/novnc --cert "${CERT}" \
		"127.0.0.1:${WEB_PORT}" "127.0.0.1:${VNC_PORT}" &
	WEBSOCKIFY_PID=$!
	sleep 2

	if curl -sfk "https://127.0.0.1:${WEB_PORT}/vnc.html" -o /dev/null; then
		echo "noVNC is being served over HTTPS"
	else
		echo "noVNC is not reachable over HTTPS" >&2
		rc=1
	fi

	echo "=== weston log ==="
	cat "${WESTON_LOG}" || true

	kill "${WEBSOCKIFY_PID}" 2>/dev/null || true
	kill "${WESTON_PID}" 2>/dev/null || true

	if [ "${rc}" -eq 0 ]; then
		echo "PASS: weston + patched neatvnc + noVNC came up"
	else
		echo "FAIL" >&2
	fi
	exit "${rc}"
	;;
*)
	exec "$@"
	;;
esac

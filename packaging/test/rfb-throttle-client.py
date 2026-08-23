#!/usr/bin/env python3
"""An RFB client that reads at a capped rate, to see how the server degrades.

The point is to compare the two things the server can do when it cannot keep
up. Reading the socket slowly is not a simulation: TCP applies real
backpressure, the server's writes block, and its fence responses come back late,
which is exactly what a bandwidth-limited link looks like from the server's
side.

The client speaks the Fence and ContinuousUpdates pseudo-encodings, which is
what feeds the server's bandwidth estimator, and reports the frame rate and
frame size it actually observed.

  --rate    link capacity to emulate, in Mb/s
  --seconds how long to measure for
"""

import argparse
import socket
import struct
import sys
import time

RFB_ENCODING_OPEN_H264 = 50
RFB_ENCODING_FENCE = -312
RFB_ENCODING_CONTINUOUSUPDATES = -313
RFB_ENCODING_QUALITY_0 = -32

MSG_SET_ENCODINGS = 2
MSG_FRAMEBUFFER_UPDATE_REQUEST = 3
MSG_ENABLE_CONTINUOUS_UPDATES = 150
MSG_FENCE = 248

SRV_FRAMEBUFFER_UPDATE = 0
SRV_END_OF_CONTINUOUS_UPDATES = 150
SRV_FENCE = 248

FENCE_REQUEST = 1 << 31
FENCE_MASK = (1 << 0) | (1 << 1) | (1 << 2)


class Throttle:
    """Lets bytes through at a fixed rate, blocking when they arrive faster."""

    def __init__(self, bytes_per_second):
        self.rate = bytes_per_second
        self.start = time.monotonic()
        self.total = 0

    def account(self, count):
        self.total += count
        if self.rate <= 0:
            return
        due = self.start + self.total / self.rate
        delay = due - time.monotonic()
        if delay > 0:
            time.sleep(delay)


class Client:
    def __init__(self, sock, throttle):
        self.sock = sock
        self.throttle = throttle
        self.buf = b""

    def recv(self, count):
        while len(self.buf) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("server closed the connection")
            self.buf += chunk
            self.throttle.account(len(chunk))
        out, self.buf = self.buf[:count], self.buf[count:]
        return out


def handshake(sock, cli):
    version = cli.recv(12)
    if not version.startswith(b"RFB "):
        raise RuntimeError(f"not an RFB server: {version!r}")
    sock.sendall(b"RFB 003.008\n")

    n = cli.recv(1)[0]
    if n == 0:
        reason_len = struct.unpack(">I", cli.recv(4))[0]
        raise RuntimeError(cli.recv(reason_len).decode(errors="replace"))
    types = cli.recv(n)
    if 1 not in types:
        raise RuntimeError(f"server wants authentication: {list(types)}")
    sock.sendall(bytes([1]))

    if struct.unpack(">I", cli.recv(4))[0] != 0:
        raise RuntimeError("security handshake failed")

    sock.sendall(bytes([1]))                      # shared
    width, height = struct.unpack(">HH", cli.recv(4))
    cli.recv(16)                                  # pixel format
    name_len = struct.unpack(">I", cli.recv(4))[0]
    cli.recv(name_len)
    return width, height


def set_encodings(sock, encodings):
    msg = struct.pack(">BBH", MSG_SET_ENCODINGS, 0, len(encodings))
    msg += b"".join(struct.pack(">i", e) for e in encodings)
    sock.sendall(msg)


def enable_continuous_updates(sock, width, height):
    sock.sendall(struct.pack(">BBHHHH", MSG_ENABLE_CONTINUOUS_UPDATES, 1,
                             0, 0, width, height))


def handle_fence(sock, cli):
    cli.recv(3)                                   # padding
    flags = struct.unpack(">I", cli.recv(4))[0]
    length = cli.recv(1)[0]
    payload = cli.recv(length)

    if not flags & FENCE_REQUEST:
        return
    # Echo it straight back, minus the request bit. Because we are reading
    # slowly, this response is already as late as the link makes it.
    sock.sendall(struct.pack(">BBBBIB", MSG_FENCE, 0, 0, 0,
                             flags & FENCE_MASK, length) + payload)


def handle_update(cli):
    cli.recv(1)                                   # padding
    n_rects = struct.unpack(">H", cli.recv(2))[0]
    total = 0
    for _ in range(n_rects):
        cli.recv(8)                               # x, y, w, h
        encoding = struct.unpack(">i", cli.recv(4))[0]
        if encoding != RFB_ENCODING_OPEN_H264:
            raise RuntimeError(f"unexpected encoding {encoding}")
        length, _flags = struct.unpack(">II", cli.recv(8))
        cli.recv(length)
        total += length
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5900)
    ap.add_argument("--rate", type=float, default=5.0,
                    help="link capacity to emulate, Mb/s (0 for unlimited)")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--quality", type=int, default=6)
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=30)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    throttle = Throttle(args.rate * 1e6 / 8.0)
    cli = Client(sock, throttle)

    width, height = handshake(sock, cli)
    set_encodings(sock, [
        RFB_ENCODING_OPEN_H264,
        RFB_ENCODING_FENCE,
        RFB_ENCODING_CONTINUOUSUPDATES,
        RFB_ENCODING_QUALITY_0 + args.quality,
    ])
    enable_continuous_updates(sock, width, height)
    sock.sendall(struct.pack(">BBHHHH", MSG_FRAMEBUFFER_UPDATE_REQUEST, 0,
                             0, 0, width, height))

    # Let the encoder settle and the estimator gather samples before measuring.
    warmup_until = time.monotonic() + 4.0
    deadline = warmup_until + args.seconds
    frames = 0
    payload_bytes = 0
    measuring = False

    try:
        while time.monotonic() < deadline:
            if not measuring and time.monotonic() >= warmup_until:
                measuring = True
                frames = 0
                payload_bytes = 0
                measure_start = time.monotonic()

            kind = cli.recv(1)[0]
            if kind == SRV_FRAMEBUFFER_UPDATE:
                got = handle_update(cli)
                if measuring:
                    frames += 1
                    payload_bytes += got
            elif kind == SRV_FENCE:
                handle_fence(sock, cli)
            elif kind == SRV_END_OF_CONTINUOUS_UPDATES:
                pass
            else:
                raise RuntimeError(f"unexpected message type {kind}")
    except (EOFError, socket.timeout) as e:
        print(f"stopped early: {e}", file=sys.stderr)
    finally:
        sock.close()

    if not measuring:
        print("never reached the measurement window", file=sys.stderr)
        return 1

    elapsed = time.monotonic() - measure_start
    fps = frames / elapsed if elapsed else 0.0
    mbps = payload_bytes * 8.0 / elapsed * 1e-6 if elapsed else 0.0
    per_frame = payload_bytes / frames if frames else 0.0

    print(f"{args.label:<28} cap {args.rate:5.1f} Mb/s | "
          f"{fps:5.1f} fps | {mbps:6.2f} Mb/s delivered | "
          f"{per_frame/1000:7.1f} kB/frame | {frames} frames in {elapsed:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())

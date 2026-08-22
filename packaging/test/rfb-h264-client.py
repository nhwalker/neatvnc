#!/usr/bin/env python3
"""Minimal RFB client that asserts a server negotiates the open-h264 encoding.

It performs an RFB 3.8 handshake, advertises open-h264 ahead of raw, asks for a
full framebuffer update and checks that what comes back is an open-h264 rect
carrying a decodable Annex B keyframe.

This exercises the whole software-frame H.264 path -- encoder selection, the
sw-frame gate in choose_frame_encoding(), AVFrame wrapping, packetisation and
RFB framing -- without needing a GPU, as long as the server is pointed at a
software encoder via NEATVNC_H264_NVENC_CODEC.
"""

import argparse
import socket
import struct
import sys

RFB_ENCODING_RAW = 0
RFB_ENCODING_OPEN_H264 = 50

NAL_TYPE_IDR = 5
NAL_TYPE_SPS = 7

PROFILE_IDC_BASELINE = 66


class ProtocolError(Exception):
    pass


def recv_exactly(sock, count):
    buf = b""
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            raise ProtocolError(
                f"connection closed after {len(buf)} of {count} bytes")
        buf += chunk
    return buf


def handshake(sock):
    version = recv_exactly(sock, 12)
    if not version.startswith(b"RFB 003."):
        raise ProtocolError(f"unexpected protocol version {version!r}")
    sock.sendall(b"RFB 003.008\n")

    n_types = recv_exactly(sock, 1)[0]
    if n_types == 0:
        reason_len = struct.unpack(">I", recv_exactly(sock, 4))[0]
        reason = recv_exactly(sock, reason_len)
        raise ProtocolError(f"server rejected connection: {reason!r}")

    types = recv_exactly(sock, n_types)
    if 1 not in types:
        raise ProtocolError(
            f"server does not offer 'None' security, offers {list(types)}")

    sock.sendall(bytes([1]))

    result = struct.unpack(">I", recv_exactly(sock, 4))[0]
    if result != 0:
        raise ProtocolError(f"security handshake failed with result {result}")

    # ClientInit: shared
    sock.sendall(bytes([1]))

    width, height = struct.unpack(">HH", recv_exactly(sock, 4))
    recv_exactly(sock, 16)  # pixel format
    name_len = struct.unpack(">I", recv_exactly(sock, 4))[0]
    name = recv_exactly(sock, name_len).decode("utf-8", "replace")

    return width, height, name


def set_encodings(sock, encodings):
    msg = struct.pack(">BBH", 2, 0, len(encodings))
    msg += b"".join(struct.pack(">i", e) for e in encodings)
    sock.sendall(msg)


def request_update(sock, width, height, incremental=0):
    sock.sendall(
        struct.pack(">BBHHHH", 3, incremental, 0, 0, width, height))


def read_h264_rect(sock, expect_encoding):
    """Reads one framebuffer update and returns (flags, payload)."""
    msg_type = recv_exactly(sock, 1)[0]
    if msg_type != 0:
        raise ProtocolError(
            f"expected a framebuffer update, got message type {msg_type}")

    recv_exactly(sock, 1)  # padding
    n_rects = struct.unpack(">H", recv_exactly(sock, 2))[0]
    if n_rects == 0:
        raise ProtocolError("framebuffer update contained no rects")

    x, y, w, h = struct.unpack(">HHHH", recv_exactly(sock, 8))
    encoding = struct.unpack(">i", recv_exactly(sock, 4))[0]

    if encoding != expect_encoding:
        raise ProtocolError(
            f"server chose encoding {encoding}, expected {expect_encoding}")

    length, flags = struct.unpack(">II", recv_exactly(sock, 8))
    payload = recv_exactly(sock, length)

    print(f"rect {w}x{h}+{x}+{y} encoding={encoding} "
          f"flags=0x{flags:x} payload={length} bytes")

    return flags, payload


def iter_nal_units(payload):
    """Yields the payload of each Annex B NAL unit."""
    starts = []
    i = 0
    while i < len(payload) - 3:
        if payload[i:i + 3] == b"\x00\x00\x01":
            starts.append(i + 3)
            i += 3
        elif payload[i:i + 4] == b"\x00\x00\x00\x01":
            starts.append(i + 4)
            i += 4
        else:
            i += 1

    for index, start in enumerate(starts):
        end = len(payload)
        if index + 1 < len(starts):
            # Trim the next start code, which is 3 or 4 bytes long.
            end = starts[index + 1] - 3
            if end > 0 and payload[end - 1] == 0:
                end -= 1
        yield payload[start:end]


def check_keyframe(payload):
    if not payload.startswith(b"\x00\x00\x00\x01") and \
            not payload.startswith(b"\x00\x00\x01"):
        raise ProtocolError("payload does not start with an Annex B start code")

    seen = {}
    for nal in iter_nal_units(payload):
        if not nal:
            continue
        nal_type = nal[0] & 0x1f
        seen.setdefault(nal_type, nal)

    print("NAL unit types present: "
          + ", ".join(str(t) for t in sorted(seen)))

    if NAL_TYPE_SPS not in seen:
        raise ProtocolError("no SPS in the first packet; the client would have "
                            "nothing to configure its decoder with")
    if NAL_TYPE_IDR not in seen:
        raise ProtocolError("first packet is not a keyframe")

    sps = seen[NAL_TYPE_SPS]
    profile_idc = sps[1]
    constraint_flags = sps[2]
    level_idc = sps[3]

    print(f"SPS profile_idc={profile_idc} constraints=0x{constraint_flags:02x} "
          f"level_idc={level_idc}")

    if profile_idc != PROFILE_IDC_BASELINE:
        raise ProtocolError(
            f"open-h264 requires baseline profile, got profile_idc "
            f"{profile_idc}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5900)
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()

    sock = socket.create_connection((args.host, args.port), args.timeout)
    sock.settimeout(args.timeout)

    try:
        width, height, name = handshake(sock)
        print(f"connected to '{name}', {width}x{height}")

        # open-h264 first, raw as the fallback the server would pick if it
        # decided H.264 was not usable.
        set_encodings(sock, [RFB_ENCODING_OPEN_H264, RFB_ENCODING_RAW])
        request_update(sock, width, height)

        _, payload = read_h264_rect(sock, RFB_ENCODING_OPEN_H264)
        check_keyframe(payload)
    except ProtocolError as err:
        print(f"FAIL: {err}", file=sys.stderr)
        return 1
    finally:
        sock.close()

    print("PASS: server negotiated open-h264 and sent a baseline keyframe")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""RFB client that checks a server's open-h264 output.

It performs an RFB 3.8 handshake, advertises open-h264 ahead of raw, collects
one or more framebuffer updates and checks what comes back:

  * every rect uses the open-h264 encoding, not raw;
  * the first packet is a keyframe carrying SPS, PPS and an IDR in baseline
    profile, which is what noVNC's WebCodecs decoder needs to configure itself;
  * with --frames > 1, later packets carry non-IDR slices, i.e. the encoder is
    producing P-frames rather than a stream of keyframes;
  * with --verify-pattern, the stream decodes to the picture the server drew.

The pattern check is the only one that looks at pixels. A well-formed bitstream
proves nothing about stride or channel order -- both produce perfectly valid
H.264 of the wrong image -- so it decodes the stream with ffmpeg and compares
the quadrant colours of h264-test-server.c's pattern.

This exercises the whole software-frame H.264 path without needing a GPU, as
long as the server is pointed at a software encoder via
NEATVNC_H264_NVENC_CODEC.
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile

RFB_ENCODING_RAW = 0
RFB_ENCODING_OPEN_H264 = 50

NAL_TYPE_NON_IDR = 1
NAL_TYPE_IDR = 5
NAL_TYPE_SPS = 7
NAL_TYPE_PPS = 8

PROFILE_IDC_BASELINE = 66

# Must match packaging/test/h264-test-server.c
PATTERN_WIDTH = 640
PATTERN_HEIGHT = 480

# Quadrant centres, plus samples hard against the left and right edges. The
# edge samples are what catch a small stride error: the server fills the
# padding beyond the image with magenta, so a row that is read a few pixels too
# wide drags that colour into view long before the picture looks obviously
# skewed.
PATTERN_SAMPLES = [
    ("top left", 160, 120, (255, 0, 0)),
    ("top right", 480, 120, (0, 255, 0)),
    ("bottom left", 160, 360, (0, 0, 255)),
    ("bottom right", 480, 360, (255, 255, 0)),
    ("left edge, top half", 5, 120, (255, 0, 0)),
    ("right edge, top half", 634, 120, (0, 255, 0)),
    ("right edge, bottom half", 634, 360, (255, 255, 0)),
]

# Generous enough for 4:2:0 subsampling and an RGB -> YUV -> RGB round trip,
# tight enough that a swapped channel (a 255 difference) cannot slip through.
COLOUR_TOLERANCE = 40

SAMPLE_RADIUS = 4


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


def nal_types(payload):
    seen = {}
    for nal in iter_nal_units(payload):
        if nal:
            seen.setdefault(nal[0] & 0x1f, nal)
    return seen


def check_keyframe(payload):
    if not payload.startswith(b"\x00\x00\x00\x01") and \
            not payload.startswith(b"\x00\x00\x01"):
        raise ProtocolError("payload does not start with an Annex B start code")

    seen = nal_types(payload)

    print("NAL unit types present: "
          + ", ".join(str(t) for t in sorted(seen)))

    if NAL_TYPE_SPS not in seen:
        raise ProtocolError("no SPS in the first packet; the client would have "
                            "nothing to configure its decoder with")
    if NAL_TYPE_PPS not in seen:
        raise ProtocolError("no PPS in the first packet")
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


def check_interframes(payloads):
    """Later packets must carry P-frames, not a stream of keyframes."""
    inter = [i for i, p in enumerate(payloads[1:], start=1)
             if NAL_TYPE_NON_IDR in nal_types(p)]

    if not inter:
        raise ProtocolError(
            f"none of the {len(payloads) - 1} packets after the first carried "
            "a non-IDR slice; the encoder is only producing keyframes")

    keyframes = [i for i, p in enumerate(payloads)
                 if NAL_TYPE_IDR in nal_types(p)]

    print(f"{len(payloads)} packets: keyframes at {keyframes}, "
          f"{len(inter)} with P-frames")


def decode_stream(payloads):
    """Decodes the concatenated packets to a list of RGB frames."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise ProtocolError(
            "--verify-pattern needs the ffmpeg binary to decode the stream")

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "stream.h264")
        with open(path, "wb") as f:
            f.write(b"".join(payloads))

        # fps_mode passthrough matters: without it ffmpeg conforms the
        # output to a constant frame rate and silently drops frames, because
        # the raw stream carries no sensible timing.
        result = subprocess.run(
            [ffmpeg, "-v", "error", "-i", path, "-fps_mode", "passthrough",
             "-pix_fmt", "rgb24", "-f", "rawvideo", "-"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)

    if result.returncode != 0:
        raise ProtocolError(
            "ffmpeg failed to decode the stream: "
            + result.stderr.decode("utf-8", "replace").strip())

    frame_size = PATTERN_WIDTH * PATTERN_HEIGHT * 3
    raw = result.stdout

    if len(raw) < frame_size:
        raise ProtocolError(
            f"decoded {len(raw)} bytes, less than one "
            f"{PATTERN_WIDTH}x{PATTERN_HEIGHT} frame")

    return [raw[i:i + frame_size]
            for i in range(0, len(raw) - frame_size + 1, frame_size)]


def sample(frame, x, y):
    """Mean RGB of a small block, to ride over subsampling artefacts."""
    totals = [0, 0, 0]
    count = 0
    for dy in range(-SAMPLE_RADIUS, SAMPLE_RADIUS + 1):
        row = (y + dy) * PATTERN_WIDTH
        for dx in range(-SAMPLE_RADIUS, SAMPLE_RADIUS + 1):
            offset = (row + x + dx) * 3
            totals[0] += frame[offset]
            totals[1] += frame[offset + 1]
            totals[2] += frame[offset + 2]
            count += 1
    return tuple(t // count for t in totals)


def check_pattern(payloads):
    frames = decode_stream(payloads)
    print(f"decoded {len(frames)} frames of "
          f"{PATTERN_WIDTH}x{PATTERN_HEIGHT}")

    if len(frames) < len(payloads):
        raise ProtocolError(
            f"sent {len(payloads)} packets but only {len(frames)} frames "
            "came out of the decoder")

    frame = frames[-1]
    failures = []

    for name, x, y, expected in PATTERN_SAMPLES:
        got = sample(frame, x, y)
        delta = max(abs(a - b) for a, b in zip(got, expected))
        status = "ok" if delta <= COLOUR_TOLERANCE else "WRONG"
        print(f"  {name:<24} at ({x},{y}): got rgb{got}, "
              f"expected rgb{expected}, max delta {delta} [{status}]")
        if delta > COLOUR_TOLERANCE:
            failures.append(name)

    if failures:
        raise ProtocolError(
            "sampled colours are wrong at: " + ", ".join(failures)
            + ". A skew points at a stride mix-up (neatvnc counts pixels, "
              "FFmpeg counts bytes); swapped quadrants point at the DRM "
              "fourcc to AVPixelFormat mapping.")

    if len(frames) > 1 and frames[-1] == frames[-2]:
        raise ProtocolError(
            "the last two decoded frames are identical, but the server moves "
            "a marker on every frame; the stream is not carrying updates")

    print("pattern matches: geometry, stride and channel order are all correct")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5900)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--frames", type=int, default=1,
                        help="number of framebuffer updates to collect")
    parser.add_argument("--verify-pattern", action="store_true",
                        help="decode the stream and compare it against the "
                             "pattern from h264-test-server.c")
    parser.add_argument("--out", help="write the elementary stream here")
    args = parser.parse_args()

    sock = socket.create_connection((args.host, args.port), args.timeout)
    sock.settimeout(args.timeout)

    payloads = []

    try:
        width, height, name = handshake(sock)
        print(f"connected to '{name}', {width}x{height}")

        # open-h264 first, raw as the fallback the server would pick if it
        # decided H.264 was not usable.
        set_encodings(sock, [RFB_ENCODING_OPEN_H264, RFB_ENCODING_RAW])

        for i in range(args.frames):
            request_update(sock, width, height, incremental=1 if i else 0)
            _, payload = read_h264_rect(sock, RFB_ENCODING_OPEN_H264)
            payloads.append(payload)

        check_keyframe(payloads[0])

        if len(payloads) > 1:
            check_interframes(payloads)

        if args.out:
            with open(args.out, "wb") as f:
                f.write(b"".join(payloads))
            print(f"wrote {args.out}")

        if args.verify_pattern:
            check_pattern(payloads)
    except ProtocolError as err:
        print(f"FAIL: {err}", file=sys.stderr)
        return 1
    finally:
        sock.close()

    print("PASS: server negotiated open-h264 and sent a baseline keyframe")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# Patched neatvnc with NVIDIA NVENC H.264

An RPM of neatvnc 0.9.1 that can offload H.264 encoding to an NVIDIA GPU, so
that Weston's VNC backend can serve a hardware-encoded desktop.

## Why this exists

neatvnc's only GPU H.264 encoder is the VAAPI one in
`src/enc/h264/ffmpeg-impl.c`. NVIDIA has no VAAPI *encoder* —
`nvidia-vaapi-driver` is decode-only — so on NVIDIA hardware
`h264_encoder_create()` always fails and H.264 is never offered.

There is a second obstacle: neatvnc refuses the open-h264 encoding unless the
frame is a GBM buffer object, and Weston's VNC backend never produces one. Both
of its renderer paths render into ordinary memory.

This build adds an NVENC encoder. NVENC accepts packed RGB and does both the
host-to-device transfer and the RGB-to-YUV conversion in hardware, so it needs
no GBM buffer object and no filter graph, and it consumes exactly the memory
buffers Weston already hands over. The open-h264 encoding is offered for memory
frames only when the selected encoder can actually consume them, so VAAPI and
V4L2 setups behave exactly as they do upstream.

The library keeps the `libneatvnc.so.0` soname that EPEL's neatvnc 0.9.0 ships,
so it is a drop-in replacement — **no Weston changes or rebuild are needed**.

## Building

```
docker build -f packaging/Containerfile.build --target export -o rpms .
```

That produces `neatvnc-0.9.1-1.el10.nvenc.x86_64.rpm` and its `-devel`,
`-debuginfo`, `-debugsource` and `.src.rpm` companions in `rpms/`. The build
runs upstream's unit tests in `%check`, verifies the soname, and runs an
open-h264 integration test against the installed library.

To build outside a container, on a machine that already has the repositories
and build dependencies:

```
packaging/build-rpm.sh [output-directory]
```

Repositories are set up by `packaging/repos.sh`:

| Repository | Provides |
| --- | --- |
| ubi10 baseos/appstream/CRB | base toolchain, gnutls, gmp, pixman |
| EPEL 10 | `aml`, and for the test image weston, websockify |
| RPM Fusion free (EL10) | `ffmpeg` 7.x — the only EL10 build with `--enable-nvenc` |
| Rocky 10 | mesa-libgbm, libdrm, turbojpeg, nettle |

Nothing from CUDA or the NVENC SDK is needed at build time: FFmpeg dlopens
`libnvidia-encode.so.1` and `libcuda.so.1` at runtime.

If you build behind a TLS-inspecting proxy, drop the proxy's CA certificate
into `packaging/ca-certificates/` — see the README there.

## Running

```
docker build -f packaging/Containerfile.test -t neatvnc-nvenc-test .
docker run --rm --gpus all \
    -e NVIDIA_DRIVER_CAPABILITIES=compute,video,utility \
    -e VNC_PASSWORD=secret \
    -p 6080:6080 neatvnc-nvenc-test
```

Then open <https://localhost:6080/vnc.html> and log in as `root`.

`NVIDIA_DRIVER_CAPABILITIES` must include `video` — that is the capability that
makes nvidia-container-toolkit inject `libnvidia-encode.so.1`. Without it
NVENC is simply absent and neatvnc falls back to raw/tight.

To deploy the RPM into an existing image instead, install it and keep EPEL from
replacing it:

```
printf 'excludepkgs=neatvnc,neatvnc-devel\n' >> /etc/yum.repos.d/epel.repo
rpm -Uvh neatvnc-0.9.1-1.el10.nvenc.x86_64.rpm
```

## Things that will bite you

**noVNC must be 1.6.0 or newer.** Support for the open-h264 encoding first
shipped in noVNC 1.6.0 and detection improved in 1.7.0. EPEL 10 packages
1.5.0, which has no H.264 decoder at all, so the test image installs noVNC from
its own release instead of the RPM.

**noVNC must be served over HTTPS.** It only offers the H.264 encoding when the
WebCodecs `VideoDecoder` API exists, and that API is restricted to secure
contexts. Over plain HTTP on a LAN address you silently get Tight instead. A
self-signed certificate is enough; `http://localhost` also counts as secure.

**The browser needs H.264 decoding.** Chrome, Edge and Firefox have it.
Chromium builds without proprietary codecs — including the one Playwright
ships — report `VideoDecoder.isConfigSupported({codec: 'avc1.42401f'})` as
false and will use Tight.

**Encoding priority needs no configuration.** noVNC advertises
`CopyRect, H264, Tight, …` and neatvnc picks the first encoding it recognises,
ignoring CopyRect, so H.264 wins on its own. There is no URL parameter for it;
if you are getting Tight, one of the three points above is the reason.

**Frames must be a single slice.** The open-h264 encoding is consumed one NAL
unit at a time — noVNC hands each to WebCodecs as its own chunk — so a frame cut
into several slices arrives as several *partial* frames and the picture falls
apart: Chrome renders a flat green field (all-zero YUV), Firefox renders drifting
mush. Both are legal H.264 that ffmpeg reassembles without complaint, which is
why only a browser catches it. NVENC emits one slice per frame; the libx264
stand-in is pinned to one thread so that it does too.

**Weston needs a PAM login.** `vnc_handle_auth()` rejects any username other
than the user Weston runs as, so in the container that means `root` plus
whatever `VNC_PASSWORD` is set to. noVNC's `password` URL parameter is not
enough on its own because RSA-AES authentication also needs the username.

## Environment variables

| Variable | Effect |
| --- | --- |
| `NEATVNC_H264_ENCODER` | `v4l2m2m` \| `vaapi` \| `nvenc` \| `auto` (default) — pins encoder selection |
| `NEATVNC_H264_NVENC_CODEC` | libavcodec encoder name, default `h264_nvenc`. Setting it to `libx264` runs the same code path on a software encoder, which is how CI tests this without a GPU |
| `NEATVNC_H264_NVENC_FORMAT` | `rgb` (default where the encoder accepts it) or `nv12`. `nv12` moves the colour conversion from NVENC to libswscale, where the coefficients are ours to choose — try it if colours look off |
| `NVNC_LOG_LEVEL` | `error` \| `warning` (release default) \| `info` \| `debug` \| `trace`. `info` is the one that names the chosen encoder and encoding |
| `NVNC_STATS_FILE` | Path to write a per-client stream health snapshot to, as JSON, every 500 ms. Unset by default, in which case nothing is written. See below |

## Checking that it is working

```
# On the host, while the screen is updating:
nvidia-smi --query-gpu=utilization.encoder --format=csv

# In the container:
ffmpeg -hide_banner -encoders | grep nvenc     # h264_nvenc must be listed
ls /usr/lib64/libnvidia-encode.so.1            # injected by the container toolkit
```

### Scrolling and the motion search

Scrolling a dense plot is the hardest thing this stack does, and how it is
searched for decides the cost by more than an order of magnitude.

A scroll is a pure translation, so an encoder that can find the displacement
codes almost nothing. `ultrafast` uses a diamond search with a range of 16
pixels, which is ample for a desktop where things move a little and useless for
one being dragged. Past roughly 24 pixels per frame the displacement leaves the
window, prediction fails, and the picture is coded from scratch instead.

Measured through the encoder here, 1920x1200 of scrolling noise:

| Scroll | `ultrafast` alone | with `me=umh:merange=64` |
| --- | --- | --- |
| 8 px/frame | 8.5 Mb/s, 57 fps | 8.4 Mb/s, 55 fps |
| 32 px/frame | 284 Mb/s, 26 fps | 11.4 Mb/s, 51 fps |
| 64 px/frame | 284 Mb/s, 24 fps | 18.9 Mb/s, 47 fps |

The wider search is applied on the software path. Note it is *faster* on the
content that needs it, which is not the trade-off one expects: when the search
fails the encoder codes intra blocks instead, and coding that failure costs more
than finding the match would have. On content that is not scrolling it is
neither better nor worse.

It is the search pattern that matters, not the range. Widening `merange` under
the diamond search changes nothing, because a diamond cannot traverse that far.

One thing worth knowing about the content, which neatvnc can do nothing about:
odd scroll displacements cost far more than even ones. Under 4:2:0 the chroma
vector is half the luma one, so an odd shift needs chroma interpolated by half a
pixel, and interpolated noise never matches real noise. Measured at 1920x1200, a
1-pixel scroll costs 124 Mb/s against 2.6 for a 2-pixel one. Quantising scroll
offsets to even pixels is an application-side change and is worth more than
anything in this section.

### Per-client stream health

Setting `NVNC_STATS_FILE` makes neatvnc rewrite a JSON snapshot every 500 ms,
holding one record per connected client:

```json
{ "version": 1, "timestamp_ms": 1787499969126, "interval_ms": 500,
  "source_fps": 60.00,
  "clients": [ { "id": 1, "address": "127.0.0.1:51924", "username": null,
                 "encoding": "open-h264", "quality": 6,
                 "frames_encoded": 1234, "frames_dropped": 12,
                 "encoded_fps": 29.80, "dropped_fps": 0.40,
                 "skip_fraction": 0.0130, "bandwidth_bps": 8400000,
                 "min_rtt_us": 420, "inflight_bytes": 18000 } ] }
```

It is current state only, so its size is bounded by the number of clients and it
cannot grow over time. Each write goes to a temporary file in the same directory
and is renamed into place, so a reader sees either the previous snapshot or the
next one and never a partial one. Rates cover the last interval rather than the
session, because a session-long average stops responding to a change in
conditions within a minute or two.

`source_fps` is how fast the compositor is feeding buffers, and the per-client
`encoded_fps` has to be read against it. Without it there is no way to tell an
idle desktop from a server that cannot keep up: both deliver few frames, and
only one of them is a problem.

Two fields need a note. `quality` is what the client asked for, on the 0-9 scale
the RFB quality pseudo-encodings use; 10 means the client expressed no
preference at all, which the H.264 encoder treats as 6. `frames_dropped` counts
frames the congestion limiter discarded, so together with `frames_encoded` it
accounts for every frame the client could have had.

**The file is not access controlled.** Anything that can read the path can see
which clients are connected, from where, and as whom. In the test container it
is deliberately written into the directory websockify serves, which means it is
readable by anything that can reach the web port, before authenticating. That is
a reasonable trade on a trusted network and nowhere else; leave the variable
unset to turn it off.

### The viewer page

`viewer.html` is a custom page built on noVNC's library rather than its stock UI.
It shows a gumball -- bright green for visually lossless, through green and
amber, to red -- and it manages picture quality itself, with no manual control.

The tier comes from the quality in effect and drops a step when the stream is
not keeping up. Quality is lowered within a second of the delivered frame rate
falling below target while the server is skipping, and raised one step at a time
after ten seconds of clean running, floored at 3. The asymmetry matters: every
change re-sends `SetEncodings`, which rebuilds the H.264 encoder and costs a key
frame.

Note what "not keeping up" means here: the delivered frame rate measured against
what the compositor is actually producing. Neither half works alone. The skip
ratio does not, because a compositor offering 60 Hz down a link that comfortably
carries 30 fps skips half of every second forever on a stream that is fine. The
delivered rate does not either, because an idle desktop delivers almost nothing
and there is nothing wrong with it.

Read together they cover both ways this goes wrong: a congested link, where
frames are encoded and then discarded, and a server too slow to encode, where
they are never produced at all. The second has no skipped frames to show for
itself, so any rule that requires skipping misses it -- and lowering quality is
exactly what would help.

Running the server with `NVNC_LOG_LEVEL=info` logs which encoder was selected
(`Using h264_nvenc for H.264 encoding`) and which encoding each client got
(`Choosing open-h264 encoding for client`). Both are INFO-level, and a release
build defaults to WARNING, so without the variable you will see neither.
Accepted values are `error`, `warning`, `info`, `debug` and `trace`.

## End-to-end test

`e2e/` holds a Java 21 / Gradle / Testcontainers test that runs this image,
points real Chrome at it through Selenium, and checks that the browser
negotiates and renders H.264 — the one thing the Python tests cannot cover,
because they are not a browser.

```
docker build -f packaging/Containerfile.build --target export -o rpms .
docker build -f packaging/Containerfile.test -t neatvnc-nvenc-test:e2e .
cd e2e && ./gradlew test && ./gradlew allureSingleFileReport
```

It asserts from both ends: the server log says it chose open-h264, and the
browser reports that H.264 rects dominate what it actually decoded, that it
*could* have decoded H.264 (so a fallback would have been a real fallback), and
that the picture changes over time rather than freezing on the first keyframe.

The evidence is an Allure report at `e2e/build/allure-report/index.html` —
one self-contained file — with the session recording and a five-second clip of
the window the assertions cover embedded in it. `/usr/share/novnc/e2e.html`
displays the encoding in use in large type, so the recording says what it is
showing.

## Testing without a GPU

```
packaging/test/selftest-draw.sh          # protocol level; needs nvnc-draw on PATH
packaging/test/selftest-pattern.sh       # pixel level; needs the ffmpeg binary
packaging/test/rfb-h264-client.py        # against any running neatvnc server
docker run --rm -e VNC_PASSWORD=x neatvnc-nvenc-test selftest
```

Set `NEATVNC_BUILD_DIR=build` to run against a meson build tree instead of the
installed library.

`rfb-h264-client.py` handshakes, advertises open-h264 ahead of raw, and asserts
the reply is an open-h264 rect carrying an Annex B keyframe with SPS, PPS and an
IDR in baseline profile — which is what noVNC's decoder needs. With `--frames N`
it also requires later packets to carry non-IDR slices, so a stream of nothing
but keyframes fails.

`selftest-pattern.sh` is the only test that looks at pixels. `h264-test-server.c`
serves four saturated quadrants from a frame buffer whose stride is wider than
the image; the client decodes the stream with ffmpeg and compares. That
combination is deliberate:

- a **stride** mistake — neatvnc counts stride in pixels, FFmpeg's `linesize` in
  bytes — skews every row, and the padding is filled with magenta so it shows up
  at the edges immediately;
- a **channel order** mistake in the DRM fourcc to `AVPixelFormat` mapping swaps
  the red and blue quadrants.

Both produce perfectly valid H.264 of the wrong picture, so nothing short of
decoding and comparing catches them. Both failure modes were injected
deliberately to confirm the test fails when it should.

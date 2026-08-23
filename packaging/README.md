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

## Checking that it is working

```
# On the host, while the screen is updating:
nvidia-smi --query-gpu=utilization.encoder --format=csv

# In the container:
ffmpeg -hide_banner -encoders | grep nvenc     # h264_nvenc must be listed
ls /usr/lib64/libnvidia-encode.so.1            # injected by the container toolkit
```

Running the server with `NVNC_LOG_LEVEL=debug` logs which encoder was selected
(`Using h264_nvenc for H.264 encoding`) and which encoding each client got.

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

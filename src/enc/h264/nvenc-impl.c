/*
 * Copyright (c) 2021 - 2024 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* NVENC based H.264 encoder.
 *
 * Unlike the VAAPI implementation in ffmpeg-impl.c, this one consumes frame
 * buffers that live in main memory. NVENC takes packed RGB input directly and
 * performs both the host to device transfer and the RGB to YUV conversion on
 * the GPU, so no GBM buffer object, dma-buf import or filter graph is needed.
 *
 * This is what makes the encoder usable with compositors that render into
 * ordinary memory buffers, such as Weston's VNC backend.
 */

#include "enc/h264-encoder.h"
#include "neatvnc.h"
#include "fb.h"
#include "sys/queue.h"
#include "vec.h"
#include "usdt.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <aml.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <libdrm/drm_fourcc.h>

#define DEFAULT_CODEC_NAME "h264_nvenc"

/* Only ever used to keep the encoder's level calculation sane; frames are
 * timestamped individually and may arrive at any rate below this.
 */
#define NOMINAL_FRAMERATE 60

struct fb_queue_entry {
	struct nvnc_fb* fb;
	TAILQ_ENTRY(fb_queue_entry) link;
};

TAILQ_HEAD(fb_queue, fb_queue_entry);

struct h264_encoder_nvenc {
	struct h264_encoder base;

	uint32_t width;
	uint32_t height;
	uint32_t format;

	AVRational timebase;

	/* Pixel format of the incoming frame buffers */
	enum AVPixelFormat src_format;

	/* Pixel format that is handed to the encoder. Equal to src_format when
	 * the encoder takes packed RGB directly.
	 */
	enum AVPixelFormat enc_format;

	AVCodecContext* codec_ctx;

	/* Only used when enc_format != src_format */
	struct SwsContext* sws;
	AVFrame* conv_frame;

	struct fb_queue fb_queue;

	struct aml_work* work;
	struct nvnc_fb* current_fb;
	struct vec current_packet;
	bool current_frame_is_keyframe;

	bool please_destroy;
};

struct h264_encoder_impl h264_encoder_nvenc_impl;

static enum AVPixelFormat drm_to_av_pixel_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return AV_PIX_FMT_BGR0;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return AV_PIX_FMT_RGB0;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		return AV_PIX_FMT_0BGR;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		return AV_PIX_FMT_0RGB;
	}

	return AV_PIX_FMT_NONE;
}

static const char* nvenc_codec_name(void)
{
	const char* name = getenv("NEATVNC_H264_NVENC_CODEC");
	return (name && name[0]) ? name : DEFAULT_CODEC_NAME;
}

/* NVENC's preset is where its search effort lives: unlike libx264 it exposes no
 * motion estimation controls at all, so p1 through p7 is the only way to ask it
 * to look harder. That matters for scrolling content, where too narrow a search
 * makes prediction fail and the picture get coded from scratch -- on the
 * software path that costs a factor of twenty-five, and whether NVENC has the
 * same cliff at p1 is not something that can be answered without the hardware.
 *
 * The defaults are unchanged, so this alters nothing until somebody sets it.
 * It exists so the ladder can be walked on a machine with a GPU without
 * rebuilding.
 */
static const char* nvenc_preset(void)
{
	const char* value = getenv("NEATVNC_H264_NVENC_PRESET");
	return (value && value[0]) ? value : "p1";
}

static const char* nvenc_tune(void)
{
	const char* value = getenv("NEATVNC_H264_NVENC_TUNE");
	return (value && value[0]) ? value : "ull";
}

static bool codec_accepts_pix_fmt(const AVCodec* codec,
		enum AVPixelFormat wanted)
{
	const enum AVPixelFormat* formats = NULL;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
	int n_formats = 0;
	int rc = avcodec_get_supported_config(NULL, codec,
			AV_CODEC_CONFIG_PIX_FORMAT, 0,
			(const void**)&formats, &n_formats);
	if (rc < 0)
		return false;

	/* A NULL list means that there are no restrictions. */
	if (!formats)
		return true;

	for (int i = 0; i < n_formats; ++i)
		if (formats[i] == wanted)
			return true;
#else
	formats = codec->pix_fmts;
	if (!formats)
		return true;

	for (int i = 0; formats[i] != AV_PIX_FMT_NONE; ++i)
		if (formats[i] == wanted)
			return true;
#endif

	return false;
}

/* NVENC converts packed RGB to YUV on the GPU, which is what we want, but the
 * conversion is done with the driver's own coefficients. Setting
 * NEATVNC_H264_NVENC_FORMAT=nv12 moves the conversion to libswscale, where the
 * coefficients are ours to choose, at the cost of some CPU time.
 *
 * NEATVNC_H264_NVENC_FORMAT=rgb is a preference, not an override: an encoder
 * that cannot take the frame buffer's format still gets NV12, because handing
 * it something it rejects would only produce an encoder that fails to open.
 */
static enum AVPixelFormat choose_encoder_format(const AVCodec* codec,
		enum AVPixelFormat src_format)
{
	const char* choice = getenv("NEATVNC_H264_NVENC_FORMAT");

	if (choice && strcmp(choice, "nv12") == 0)
		return AV_PIX_FMT_NV12;

	if (codec_accepts_pix_fmt(codec, src_format))
		return src_format;

	if (choice && strcmp(choice, "rgb") == 0)
		nvnc_log(NVNC_LOG_WARNING,
				"%s does not accept %s input; converting to nv12 instead",
				codec->name, av_get_pix_fmt_name(src_format));

	return AV_PIX_FMT_NV12;
}

/* Options are applied one at a time and failures are not fatal: the option set
 * below is tuned for NVENC, but the codec name is overridable so that the
 * encoder can be exercised with a software encoder where no GPU is available.
 */
static void try_set_option(struct h264_encoder_nvenc* self, const char* key,
		const char* value)
{
	if (!self->codec_ctx->priv_data)
		return;

	int rc = av_opt_set(self->codec_ctx->priv_data, key, value, 0);
	if (rc < 0)
		nvnc_log(NVNC_LOG_DEBUG,
				"Encoder does not accept option %s=%s, ignoring",
				key, value);
}

static int h264_encoder__init_codec_context(struct h264_encoder_nvenc* self,
		const AVCodec* codec, int quality)
{
	self->codec_ctx = avcodec_alloc_context3(codec);
	if (!self->codec_ctx)
		return -1;

	struct AVCodecContext* c = self->codec_ctx;
	c->width = self->width;
	c->height = self->height;
	c->time_base = self->timebase;

	/* The time base is microseconds so that frame timestamps keep their
	 * precision, but an encoder left to infer the frame rate from it
	 * concludes that we are feeding it a million frames a second. It then
	 * writes an SPS whose level cannot legally carry that macroblock rate,
	 * and strict decoders -- Chrome's WebCodecs among them -- refuse the
	 * stream outright. Declare a plausible ceiling instead.
	 */
	c->framerate = (AVRational){ NOMINAL_FRAMERATE, 1 };
	c->sample_aspect_ratio = (AVRational){1, 1};
	c->pix_fmt = self->enc_format;
	c->gop_size = INT32_MAX; /* We'll select key frames manually */
	c->max_b_frames = 0; /* B-frames are bad for latency */
	c->global_quality = quality;

	/* open-h264 requires baseline profile, so we use constrained
	 * baseline.
	 */
	c->profile = 578;

	// Encode BT.709 into the bitstream:
	c->colorspace = AVCOL_SPC_BT709;
	c->color_primaries = AVCOL_PRI_BT709;
	c->color_range = AVCOL_RANGE_MPEG;
	c->color_trc = AVCOL_TRC_BT709;

	char quality_str[16];
	snprintf(quality_str, sizeof(quality_str), "%d", quality);

	if (strstr(codec->name, "nvenc")) {
		/* Lowest latency preset, constant quantiser, no frame
		 * reordering and no output delay. forced-idr makes
		 * AV_PICTURE_TYPE_I produce a real IDR, which the open-h264
		 * encoding relies on for context resets. The preset and tune
		 * are overridable; see nvenc_preset().
		 */
		try_set_option(self, "preset", nvenc_preset());
		try_set_option(self, "tune", nvenc_tune());

		/* Two-pass motion estimation. Off by default because it costs
		 * GPU time and its value here is unmeasured; it is the other
		 * knob worth trying if a fast scroll turns out to defeat the
		 * search at whichever preset is in use.
		 */
		const char* multipass = getenv("NEATVNC_H264_NVENC_MULTIPASS");
		if (multipass && multipass[0])
			try_set_option(self, "multipass", multipass);
		try_set_option(self, "rc", "constqp");
		try_set_option(self, "qp", quality_str);
		try_set_option(self, "zerolatency", "1");
		try_set_option(self, "delay", "0");
		try_set_option(self, "forced-idr", "1");
		try_set_option(self, "bf", "0");
	} else {
		/* Only reached when NEATVNC_H264_NVENC_CODEC points this
		 * implementation at a software encoder, which is how the code
		 * path is tested where there is no NVIDIA GPU.
		 */
		try_set_option(self, "preset", "ultrafast");
		try_set_option(self, "tune", "zerolatency");
		try_set_option(self, "crf", quality_str);

		/* A wider motion search than the ultrafast preset picks.
		 *
		 * ultrafast uses a diamond search with a range of 16 pixels,
		 * which is ample for a desktop where things move a little and
		 * useless for one being scrolled. Past about 24 pixels per
		 * frame the displacement leaves the search window, prediction
		 * fails outright, and the encoder codes the picture from
		 * scratch instead -- on dense content that is the difference
		 * between eight megabits a second and three hundred.
		 *
		 * The search pattern is what matters, not the range: widening
		 * merange alone changes nothing, because the diamond cannot
		 * traverse that far. umh can.
		 *
		 * It is also faster on exactly the content that needs it, which
		 * is not the trade-off one expects. When the search fails the
		 * encoder falls back to coding intra blocks, and coding that
		 * failure costs more than finding the match would have.
		 */
		try_set_option(self, "x264-params", "me=umh:merange=64");

		/* One thread, because threading is what decides how many slices
		 * a frame is cut into. x264's zerolatency tune turns on sliced
		 * threads, which produces one slice per core; turning those off
		 * instead hands the job to frame threads, which delay the first
		 * packets. Either is legal H.264 and ffmpeg reassembles it
		 * without complaint, but the open-h264 encoding is consumed one
		 * NAL unit at a time -- noVNC hands each to WebCodecs as its own
		 * chunk -- so a multi-slice frame arrives as several partial
		 * frames and the picture falls apart. NVENC emits one slice per
		 * frame regardless; this keeps the software stand-in honest
		 * about the shape of the stream it stands in for.
		 */
		c->thread_count = 1;
	}

	return 0;
}

static int h264_encoder__init_conversion(struct h264_encoder_nvenc* self)
{
	if (self->enc_format == self->src_format)
		return 0;

	self->sws = sws_getContext(self->width, self->height, self->src_format,
			self->width, self->height, self->enc_format,
			SWS_FAST_BILINEAR, NULL, NULL, NULL);
	if (!self->sws)
		return -1;

	/* Convert into BT.709 limited range, matching what we signal in the
	 * bitstream.
	 */
	const int* rgb_coeffs = sws_getCoefficients(SWS_CS_ITU709);
	const int* yuv_coeffs = sws_getCoefficients(SWS_CS_ITU709);
	sws_setColorspaceDetails(self->sws, rgb_coeffs, 1 /* full range src */,
			yuv_coeffs, 0 /* limited range dst */, 0, 1 << 16,
			1 << 16);

	self->conv_frame = av_frame_alloc();
	if (!self->conv_frame)
		goto frame_failure;

	self->conv_frame->width = self->width;
	self->conv_frame->height = self->height;
	self->conv_frame->format = self->enc_format;

	if (av_frame_get_buffer(self->conv_frame, 0) < 0)
		goto buffer_failure;

	return 0;

buffer_failure:
	av_frame_free(&self->conv_frame);
frame_failure:
	sws_freeContext(self->sws);
	self->sws = NULL;
	return -1;
}

static struct nvnc_fb* fb_queue_dequeue(struct fb_queue* queue)
{
	if (TAILQ_EMPTY(queue))
		return NULL;

	struct fb_queue_entry* entry = TAILQ_FIRST(queue);
	TAILQ_REMOVE(queue, entry, link);
	struct nvnc_fb* fb = entry->fb;
	free(entry);

	return fb;
}

static int fb_queue_enqueue(struct fb_queue* queue, struct nvnc_fb* fb)
{
	struct fb_queue_entry* entry = calloc(1, sizeof(*entry));
	if (!entry)
		return -1;

	entry->fb = fb;
	nvnc_fb_ref(fb);
	TAILQ_INSERT_TAIL(queue, entry, link);

	return 0;
}

static int h264_encoder__schedule_work(struct h264_encoder_nvenc* self)
{
	if (self->current_fb)
		return 0;

	self->current_fb = fb_queue_dequeue(&self->fb_queue);
	if (!self->current_fb)
		return 0;

	DTRACE_PROBE1(neatvnc, h264_encode_frame_begin, self->current_fb->pts);

	self->current_frame_is_keyframe = self->base.next_frame_should_be_keyframe;
	self->base.next_frame_should_be_keyframe = false;

	return aml_start(aml_get_default(), self->work);
}

static void set_frame_type(struct h264_encoder_nvenc* self, AVFrame* frame)
{
	if (self->current_frame_is_keyframe) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
		frame->flags |= AV_FRAME_FLAG_KEY;
#else
		frame->key_frame = 1;
#endif
		frame->pict_type = AV_PICTURE_TYPE_I;
	} else {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
		frame->flags &= ~AV_FRAME_FLAG_KEY;
#else
		frame->key_frame = 0;
#endif
		frame->pict_type = AV_PICTURE_TYPE_P;
	}
}

/* Wraps the frame buffer's pixels in an AVFrame without copying them. */
static AVFrame* fb_to_avframe(struct h264_encoder_nvenc* self,
		struct nvnc_fb* fb)
{
	AVFrame* frame = av_frame_alloc();
	if (!frame)
		return NULL;

	frame->width = self->width;
	frame->height = self->height;
	frame->format = self->src_format;
	frame->sample_aspect_ratio = (AVRational){1, 1};
	frame->pts = fb->pts;

	frame->data[0] = fb->addr;
	frame->linesize[0] = fb->stride * nvnc_fb_get_pixel_size(fb);

	// sRGB:
	frame->colorspace = AVCOL_SPC_RGB;
	frame->color_primaries = AVCOL_PRI_BT709;
	frame->color_range = AVCOL_RANGE_JPEG;
	frame->color_trc = AVCOL_TRC_IEC61966_2_1;

	return frame;
}

static int h264_encoder__convert(struct h264_encoder_nvenc* self,
		const AVFrame* src)
{
	int rc = av_frame_make_writable(self->conv_frame);
	if (rc < 0)
		return rc;

	sws_scale(self->sws, (const uint8_t* const*)src->data, src->linesize, 0,
			self->height, self->conv_frame->data,
			self->conv_frame->linesize);

	self->conv_frame->pts = src->pts;
	self->conv_frame->colorspace = AVCOL_SPC_BT709;
	self->conv_frame->color_primaries = AVCOL_PRI_BT709;
	self->conv_frame->color_range = AVCOL_RANGE_MPEG;
	self->conv_frame->color_trc = AVCOL_TRC_BT709;

	return 0;
}

static int h264_encoder__encode(struct h264_encoder_nvenc* self,
		AVFrame* frame_in)
{
	int rc = avcodec_send_frame(self->codec_ctx, frame_in);
	if (rc < 0)
		return rc;

	AVPacket* packet = av_packet_alloc();
	if (!packet)
		return AVERROR(ENOMEM);

	while (1) {
		rc = avcodec_receive_packet(self->codec_ctx, packet);
		if (rc != 0)
			break;

		vec_append(&self->current_packet, packet->data, packet->size);

		packet->stream_index = 0;
		av_packet_unref(packet);
	}

	// Frame should always start with a zero:
	assert(self->current_packet.len == 0 ||
			((char*)self->current_packet.data)[0] == 0);

	av_packet_free(&packet);
	return rc == AVERROR(EAGAIN) ? 0 : rc;
}

static void h264_encoder__do_work(void* handle)
{
	struct h264_encoder_nvenc* self = aml_get_userdata(handle);

	AVFrame* frame = fb_to_avframe(self, self->current_fb);
	if (!frame) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to allocate frame");
		return;
	}

	AVFrame* frame_in = frame;
	int rc = 0;

	if (self->sws) {
		rc = h264_encoder__convert(self, frame);
		if (rc < 0) {
			nvnc_log(NVNC_LOG_ERROR,
					"Failed to convert frame for encoding");
			goto failure;
		}
		frame_in = self->conv_frame;
	}

	set_frame_type(self, frame_in);

	rc = h264_encoder__encode(self, frame_in);
	if (rc != 0) {
		char err[256];
		av_strerror(rc, err, sizeof(err));
		nvnc_log(NVNC_LOG_ERROR, "Failed to encode packet: %s", err);
	}

failure:
	av_frame_free(&frame);
}

static void h264_encoder__on_work_done(void* handle)
{
	struct h264_encoder_nvenc* self = aml_get_userdata(handle);

	uint64_t pts = nvnc_fb_get_pts(self->current_fb);
	nvnc_fb_release(self->current_fb);
	nvnc_fb_unref(self->current_fb);
	self->current_fb = NULL;

	DTRACE_PROBE1(neatvnc, h264_encode_frame_end, pts);

	if (self->please_destroy) {
		h264_encoder_destroy(&self->base);
		return;
	}

	if (self->current_packet.len == 0) {
		nvnc_log(NVNC_LOG_WARNING, "Whoops, encoded packet length is 0");
		return;
	}

	void* userdata = self->base.userdata;

	// Must make a copy of packet because the callback might destroy the
	// encoder object.
	struct vec packet;
	vec_init(&packet, self->current_packet.len);
	vec_append(&packet, self->current_packet.data,
			self->current_packet.len);

	vec_clear(&self->current_packet);
	h264_encoder__schedule_work(self);

	self->base.on_packet_ready(packet.data, packet.len, pts, userdata);
	vec_destroy(&packet);
}

static struct h264_encoder* h264_encoder_nvenc_create(uint32_t width,
		uint32_t height, uint32_t format, int quality)
{
	struct h264_encoder_nvenc* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->base.impl = &h264_encoder_nvenc_impl;

	if (vec_init(&self->current_packet, 65536) < 0)
		goto packet_failure;

	self->work = aml_work_new(h264_encoder__do_work,
			h264_encoder__on_work_done, self, NULL);
	if (!self->work)
		goto worker_failure;

	self->base.next_frame_should_be_keyframe = true;
	TAILQ_INIT(&self->fb_queue);

	self->width = width;
	self->height = height;
	self->format = format;
	self->timebase = (AVRational){1, 1000000};

	self->src_format = drm_to_av_pixel_format(format);
	if (self->src_format == AV_PIX_FMT_NONE)
		goto pix_fmt_failure;

	const char* codec_name = nvenc_codec_name();
	const AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
	if (!codec) {
		nvnc_log(NVNC_LOG_DEBUG, "%s encoder is not available",
				codec_name);
		goto codec_failure;
	}

	self->enc_format = choose_encoder_format(codec, self->src_format);

	if (h264_encoder__init_conversion(self) < 0)
		goto conversion_failure;

	if (h264_encoder__init_codec_context(self, codec, quality) < 0)
		goto codec_context_failure;

	int rc = avcodec_open2(self->codec_ctx, codec, NULL);
	if (rc != 0) {
		char err[256];
		av_strerror(rc, err, sizeof(err));
		nvnc_log(NVNC_LOG_DEBUG, "Failed to open %s: %s", codec_name,
				err);
		goto avcodec_open_failure;
	}

	nvnc_log(NVNC_LOG_INFO, "Using %s for H.264 encoding", codec_name);

	return &self->base;

avcodec_open_failure:
	avcodec_free_context(&self->codec_ctx);
codec_context_failure:
	av_frame_free(&self->conv_frame);
	sws_freeContext(self->sws);
	self->sws = NULL;
conversion_failure:
codec_failure:
pix_fmt_failure:
	aml_unref(self->work);
worker_failure:
	vec_destroy(&self->current_packet);
packet_failure:
	free(self);
	return NULL;
}

static void h264_encoder_nvenc_destroy(struct h264_encoder* base)
{
	struct h264_encoder_nvenc* self = (struct h264_encoder_nvenc*)base;

	if (self->current_fb) {
		self->please_destroy = true;
		return;
	}

	vec_destroy(&self->current_packet);
	avcodec_free_context(&self->codec_ctx);
	av_frame_free(&self->conv_frame);
	sws_freeContext(self->sws);
	aml_unref(self->work);
	free(self);
}

static void h264_encoder_nvenc_feed(struct h264_encoder* base,
		struct nvnc_fb* fb)
{
	struct h264_encoder_nvenc* self = (struct h264_encoder_nvenc*)base;

	// TODO: Add transform filter
	assert(fb->transform == NVNC_TRANSFORM_NORMAL);

	if (nvnc_fb_map(fb) < 0) {
		nvnc_log(NVNC_LOG_ERROR, "Failed to map frame buffer");
		return;
	}

	int rc __attribute__((unused)) = fb_queue_enqueue(&self->fb_queue, fb);
	assert(rc == 0); // TODO

	nvnc_fb_hold(fb);

	rc = h264_encoder__schedule_work(self);
	assert(rc == 0); // TODO
}

struct h264_encoder_impl h264_encoder_nvenc_impl = {
	.create = h264_encoder_nvenc_create,
	.destroy = h264_encoder_nvenc_destroy,
	.feed = h264_encoder_nvenc_feed,
	.accepts_sw_frames = true,
};

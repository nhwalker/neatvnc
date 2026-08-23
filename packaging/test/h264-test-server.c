/*
 * A neatvnc server that serves a known pattern, for verifying that the H.264
 * encoder produces the right pixels rather than merely a well-formed bitstream.
 *
 * Three properties of the image are chosen deliberately:
 *
 *  - The four quadrants have distinct, saturated colours. A red/blue mix-up in
 *    the DRM fourcc to AVPixelFormat mapping swaps two of them, and no two
 *    quadrants are the same under any channel permutation.
 *  - The frame buffer stride is wider than the image. Anything that confuses
 *    neatvnc's stride (in pixels) with FFmpeg's linesize (in bytes) skews every
 *    row and destroys the quadrant boundaries.
 *  - A marker moves across the middle band on every frame, so consecutive
 *    frames differ. It stays clear of the points the checker samples.
 *
 * Pair it with rfb-h264-client.py --frames N --verify-pattern.
 */

#include <neatvnc.h>
#include <aml.h>
#include <pixman.h>

#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libdrm/drm_fourcc.h>

#define WIDTH 640
#define HEIGHT 480

/* Deliberately wider than the image; see the note above. */
#define STRIDE (WIDTH + 16)

#define FRAME_PERIOD_US 50000

#define MARKER_SIZE 64
#define MARKER_TOP (HEIGHT / 2 - MARKER_SIZE / 2)
#define MARKER_STEP 24

/* 0x00RRGGBB, which is what a DRM_FORMAT_XRGB8888 pixel holds. */
#define COLOUR_TOP_LEFT 0x00ff0000     /* red */
#define COLOUR_TOP_RIGHT 0x0000ff00    /* green */
#define COLOUR_BOTTOM_LEFT 0x000000ff  /* blue */
#define COLOUR_BOTTOM_RIGHT 0x00ffff00 /* yellow */
#define COLOUR_MARKER 0x00000000       /* black */

struct test_server {
	struct nvnc* server;
	struct nvnc_display* display;
	struct nvnc_fb_pool* fb_pool;
	int frame;
};

static void on_sigint(void* obj)
{
	aml_exit(aml_get_default());
}

static void draw_pattern(uint32_t* pixels, int frame)
{
	for (int y = 0; y < HEIGHT; ++y) {
		uint32_t* row = pixels + (size_t)y * STRIDE;

		uint32_t left = y < HEIGHT / 2 ?
			COLOUR_TOP_LEFT : COLOUR_BOTTOM_LEFT;
		uint32_t right = y < HEIGHT / 2 ?
			COLOUR_TOP_RIGHT : COLOUR_BOTTOM_RIGHT;

		for (int x = 0; x < WIDTH / 2; ++x)
			row[x] = left;
		for (int x = WIDTH / 2; x < WIDTH; ++x)
			row[x] = right;

		/* Fill the padding with a colour that must never show up in a
		 * decoded frame. If it does, the stride is being misread.
		 */
		for (int x = WIDTH; x < STRIDE; ++x)
			row[x] = 0x00ff00ff;
	}

	int marker_x = (frame * MARKER_STEP) % (WIDTH - MARKER_SIZE);

	for (int y = MARKER_TOP; y < MARKER_TOP + MARKER_SIZE; ++y) {
		uint32_t* row = pixels + (size_t)y * STRIDE;
		for (int x = marker_x; x < marker_x + MARKER_SIZE; ++x)
			row[x] = COLOUR_MARKER;
	}
}

static void feed_frame(struct test_server* self)
{
	struct nvnc_fb* fb = nvnc_fb_pool_acquire(self->fb_pool);
	assert(fb);

	draw_pattern(nvnc_fb_get_addr(fb), self->frame++);

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, WIDTH, HEIGHT);
	nvnc_display_feed_buffer(self->display, fb, &damage);
	pixman_region_fini(&damage);

	nvnc_fb_unref(fb);
}

static void on_frame(void* obj)
{
	feed_frame(aml_get_userdata(obj));
}

int main(int argc, char* argv[])
{
	const char* address = argc > 1 ? argv[1] : "127.0.0.1";
	int port = argc > 2 ? atoi(argv[2]) : 5900;

	struct test_server self = { 0 };

	struct aml* aml = aml_new();
	assert(aml);
	aml_set_default(aml);

	self.fb_pool = nvnc_fb_pool_new(WIDTH, HEIGHT, DRM_FORMAT_XRGB8888,
			STRIDE);
	assert(self.fb_pool);

	self.server = nvnc_open(address, port);
	assert(self.server);

	self.display = nvnc_display_new(0, 0);
	assert(self.display);

	nvnc_add_display(self.server, self.display);
	nvnc_set_name(self.server, "h264-test-pattern");

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	aml_start(aml, sig);
	aml_unref(sig);

	/* Feed one frame up front so that a client connecting immediately has
	 * something to receive.
	 */
	feed_frame(&self);

	struct aml_ticker* ticker = aml_ticker_new(FRAME_PERIOD_US, on_frame,
			&self, NULL);
	assert(ticker);
	aml_start(aml, ticker);
	aml_unref(ticker);

	printf("serving %dx%d test pattern on %s:%d\n", WIDTH, HEIGHT, address,
			port);
	fflush(stdout);

	aml_run(aml);

	nvnc_close(self.server);
	nvnc_display_unref(self.display);
	nvnc_fb_pool_unref(self.fb_pool);
	aml_unref(aml);

	return 0;
}

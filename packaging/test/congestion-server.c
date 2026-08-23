/* A VNC server whose desktop is expensive to encode, for exercising congestion
 * control.
 *
 * The content is a scrolling field of noise, which is what a dense data plot --
 * a waterfall, a spectrogram, a packed scatter -- looks like to an encoder:
 * high entropy, nothing spatial to exploit. It is deliberately the hardest
 * realistic case, because congestion control that only works on a still desktop
 * is not worth having.
 *
 * The scroll offset advances by an even number of pixels per frame. Odd
 * displacements are far more expensive under 4:2:0 chroma subsampling, since
 * chroma then has to be interpolated by half a pixel, and that is a property of
 * the content rather than of the thing being tested here.
 *
 * Usage: congestion-server [address] [port]
 */

#include "neatvnc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <aml.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>

#define WIDTH 1280
#define HEIGHT 720
#define FRAME_PERIOD_US 33333   /* 30 Hz */
#define SCROLL_PX 2             /* even: see the comment above */

struct server {
	struct nvnc* server;
	struct nvnc_display* display;
	struct nvnc_fb_pool* fb_pool;
	uint32_t* field;
	int field_h;
	int frame;
};

static void build_field(struct server* self)
{
	self->field_h = HEIGHT + 4096;
	self->field = malloc((size_t)WIDTH * self->field_h * 4);
	assert(self->field);

	for (int y = 0; y < self->field_h; ++y) {
		for (int x = 0; x < WIDTH; ++x) {
			unsigned h = (unsigned)x * 2654435761u ^
				(unsigned)y * 40503u;
			h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
			uint32_t v = h & 0xff;
			self->field[(size_t)y * WIDTH + x] =
				(v << 16) | ((255 - v) << 8) |
				((v * 3 / 2) & 0xff);
		}
	}
}

static void on_frame(void* obj)
{
	struct server* self = aml_get_userdata(obj);

	struct nvnc_fb* fb = nvnc_fb_pool_acquire(self->fb_pool);
	assert(fb);

	uint32_t* pixels = nvnc_fb_get_addr(fb);
	int stride = nvnc_fb_get_stride(fb);
	int offset = self->frame * SCROLL_PX;

	for (int y = 0; y < HEIGHT; ++y) {
		int src = (offset + y) % self->field_h;
		memcpy(pixels + (size_t)y * stride,
				self->field + (size_t)src * WIDTH, WIDTH * 4);
	}

	self->frame++;

	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, WIDTH, HEIGHT);
	nvnc_display_feed_buffer(self->display, fb, &damage);
	pixman_region_fini(&damage);

	nvnc_fb_unref(fb);
}

static void on_sigint(void* obj)
{
	aml_exit(aml_get_default());
}

int main(int argc, char* argv[])
{
	const char* address = argc > 1 ? argv[1] : "127.0.0.1";
	int port = argc > 2 ? atoi(argv[2]) : 5900;

	struct aml* aml = aml_new();
	assert(aml);
	aml_set_default(aml);
	aml_require_workers(aml, -1);

	struct server self = {};
	build_field(&self);

	self.server = nvnc_open(address, port);
	assert(self.server);
	nvnc_set_name(self.server, "neatvnc congestion test");

	self.display = nvnc_display_new(0, 0);
	assert(self.display);
	nvnc_add_display(self.server, self.display);

	self.fb_pool = nvnc_fb_pool_new(WIDTH, HEIGHT, DRM_FORMAT_XRGB8888,
			WIDTH);
	assert(self.fb_pool);

	struct aml_ticker* ticker = aml_ticker_new(FRAME_PERIOD_US, on_frame,
			&self, NULL);
	assert(ticker);
	aml_start(aml, ticker);

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	assert(sig);
	aml_start(aml, sig);

	printf("congestion test server on %s:%d, %dx%d at %d Hz\n",
			address, port, WIDTH, HEIGHT, 1000000 / FRAME_PERIOD_US);
	fflush(stdout);

	aml_run(aml);

	aml_stop(aml, sig);
	aml_unref(sig);
	aml_stop(aml, ticker);
	aml_unref(ticker);
	nvnc_fb_pool_unref(self.fb_pool);
	nvnc_display_unref(self.display);
	nvnc_close(self.server);
	free(self.field);
	aml_unref(aml);
	return 0;
}

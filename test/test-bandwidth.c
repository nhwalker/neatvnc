#include "bandwidth.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>

#define SAMPLES_MAX 16

static void feed(struct bwe* self, int bytes, int departure, int rtt)
{
	struct bwe_sample sample = {
		.bytes = bytes,
		.departure_time = departure,
		.arrival_time = departure + rtt,
	};
	bwe_feed(self, &sample);
}

static bool near(int actual, int expected)
{
	// The estimate is a rounded double, so allow the last digit to move.
	bool ok = abs(actual - expected) <= 1;
	if (!ok)
		printf("       expected %d, got %d\n", expected, actual);
	return ok;
}

static bool test_empty(void)
{
	struct bwe* self = bwe_create(1000);
	bool ok = bwe_get_estimate(self) == 0;
	bwe_destroy(self);
	return ok;
}

/* The constructor's argument has to reach the estimator: with rtt_min at the
 * 1000 us passed in, the excess delay is 500 us per sample and the estimate is
 * 2 MB/s. Were it left at zero -- as it was before -- the excess delay would be
 * the whole round trip and the estimate would come out three times lower.
 */
static bool test_create_stores_rtt_min(void)
{
	struct bwe* self = bwe_create(1000);

	for (int i = 0; i < 4; ++i)
		feed(self, 1000, i * 10000, 1500);

	// 4000 bytes / (4 * 500 us)
	bool ok = near(bwe_get_estimate(self), 2000000);
	bwe_destroy(self);
	return ok;
}

/* Every round trip taking exactly the minimum is the ordinary case on a quiet
 * fast link, and it leaves the non-congested estimator with a zero denominator.
 * It used to divide by it, and rounding the resulting infinity into an int is
 * undefined -- in practice INT_MIN, which the server then treats as a negative
 * bandwidth and stops sending frames altogether. The estimate must stay a
 * usable non-negative number.
 */
static bool test_zero_excess_delay(void)
{
	struct bwe* self = bwe_create(500);

	for (int i = 0; i < 8; ++i)
		feed(self, 20000, i * 10000, 500);

	int estimate = bwe_get_estimate(self);

	// Falls back to what was actually delivered over the window:
	// 160000 bytes / (70500 - 500 us)
	bool ok = estimate >= 0 && near(estimate, 2285714);
	if (estimate < 0)
		printf("       negative estimate: %d\n", estimate);

	bwe_destroy(self);
	return ok;
}

/* A single sample cannot say anything about either denominator. */
static bool test_single_sample_at_min_rtt(void)
{
	struct bwe* self = bwe_create(500);
	feed(self, 20000, 0, 500);
	int estimate = bwe_get_estimate(self);
	bwe_destroy(self);
	return estimate == 0;
}

/* A minimum recorded as lower than anything since observed -- which happens,
 * because min_rtt is also fed from message timings during the handshake --
 * must not produce nonsense either.
 */
static bool test_rtt_min_below_observed(void)
{
	struct bwe* self = bwe_create(100);

	for (int i = 0; i < 8; ++i)
		feed(self, 20000, i * 10000, 500);

	// 160000 bytes / (8 * 400 us)
	bool ok = near(bwe_get_estimate(self), 50000000);
	bwe_destroy(self);
	return ok;
}

/* Spacing between packets is the signal the non-congested estimator reads:
 * total bytes over total excess delay.
 */
static bool test_non_congested(void)
{
	struct bwe* self = bwe_create(1000);

	for (int i = 0; i < 10; ++i)
		feed(self, 5000, i * 20000, 1000 + 250);

	// 50000 bytes / (10 * 250 us)
	bool ok = near(bwe_get_estimate(self), 20000000);
	bwe_destroy(self);
	return ok;
}

/* When the link is saturated the queue grows, so per-sample delay stops being
 * meaningful and the window as a whole is what counts. The larger of the two
 * estimates wins, and here that is the window one.
 */
static bool test_congested_window_wins(void)
{
	struct bwe* self = bwe_create(1000);

	// Ten packets back to back, each taking a long time to come back.
	for (int i = 0; i < 10; ++i)
		feed(self, 5000, i * 100, 50000);

	// Non-congested: 50000 bytes / (10 * 49000 us) = 102040 B/s.
	// Congested: 50000 bytes / (50900 - 1000 us) = 1002004 B/s.
	bool ok = near(bwe_get_estimate(self), 1002004);
	bwe_destroy(self);
	return ok;
}

/* Only the most recent window is kept. */
static bool test_ring_buffer_wraps(void)
{
	struct bwe* self = bwe_create(1000);

	// Old samples, far larger, which would dominate if they were retained.
	for (int i = 0; i < 8; ++i)
		feed(self, 1000000, i * 20000, 1000 + 250);

	for (int i = 0; i < SAMPLES_MAX; ++i)
		feed(self, 5000, (8 + i) * 20000, 1000 + 250);

	// 16 * 5000 bytes / (16 * 250 us)
	bool ok = near(bwe_get_estimate(self), 20000000);
	bwe_destroy(self);
	return ok;
}

/* Whatever the timings, the server multiplies this by a delay budget and
 * compares it against a byte count, so it must never come back negative.
 */
static bool test_never_negative(void)
{
	bool ok = true;

	for (int rtt_min = 0; rtt_min < 2000; rtt_min += 137) {
		struct bwe* self = bwe_create(rtt_min);

		for (int i = 0; i < 40; ++i) {
			int rtt = (i % 7) * 300;
			feed(self, 100 + i * 997, i * 1000, rtt);

			int estimate = bwe_get_estimate(self);
			if (estimate < 0) {
				printf("       negative estimate %d at rtt_min=%d, i=%d\n",
						estimate, rtt_min, i);
				ok = false;
			}
		}

		bwe_destroy(self);
	}

	return ok;
}

/* A near-zero excess delay is the case the zero guard does not catch: large
 * frames over a single microsecond of summed delay put the estimate in the
 * terabytes per second, far beyond what int can hold, and the conversion used
 * to wrap to INT_MIN. Loopback-adjacent clients -- a websockify hop on the
 * same host -- produce exactly these timings. The estimate must saturate.
 */
static bool test_estimate_saturates(void)
{
	struct bwe* self = bwe_create(50);

	// Sixteen samples of 512 KiB; every round trip at the minimum except
	// one, which is a single microsecond over.
	for (int i = 0; i < 16; ++i)
		feed(self, 512 * 1024, i * 1000, 50 + (i == 7 ? 1 : 0));

	int estimate = bwe_get_estimate(self);
	bool ok = estimate == INT_MAX;
	if (!ok)
		printf("       expected INT_MAX, got %d\n", estimate);

	bwe_destroy(self);
	return ok;
}

#define XSTR(s) STR(s)
#define STR(s) #s

#define RUN_TEST(name) ({ \
	bool ok = test_ ## name(); \
	printf("[%s] %s\n", ok ? " OK " : "FAIL", XSTR(name)); \
	ok; \
})

int main()
{
	bool ok = true;

	ok &= RUN_TEST(empty);
	ok &= RUN_TEST(create_stores_rtt_min);
	ok &= RUN_TEST(zero_excess_delay);
	ok &= RUN_TEST(single_sample_at_min_rtt);
	ok &= RUN_TEST(rtt_min_below_observed);
	ok &= RUN_TEST(non_congested);
	ok &= RUN_TEST(congested_window_wins);
	ok &= RUN_TEST(ring_buffer_wraps);
	ok &= RUN_TEST(never_negative);
	ok &= RUN_TEST(estimate_saturates);

	return ok ? 0 : 1;
}

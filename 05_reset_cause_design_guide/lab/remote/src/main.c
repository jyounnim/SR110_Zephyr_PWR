/*
 * Lab 05 - M4 side: the same accelerometer monitor + mbox notify
 * workload as Lab 03, reused here UNCHANGED. This is the confirmed,
 * working "Always-On watches, M55 wakes on event" design that Lab 00's
 * original design question (can M4 wake M55?) was ultimately answered
 * by. See the M55 side (lab/src/main.c) for the boot-time reset-cause
 * dispatch this lab adds on top of Lab 03's pattern.
 *
 * Continuously monitor the onboard accelerometer (mc3479 on I2C1) and
 * notify M55 over mbox ONLY when the motion state changes (still <->
 * active), not on every sample.
 *
 * Hardware-confirmed (2026-09-21, Lab 03): shaking the board produces a
 * clean STILL/ACTIVE alternation on the M55 side with no missed or
 * duplicate events, using ACCEL_THRESHOLD_MILLI_G = 2000 as-is (no
 * retuning needed for this board/mounting).
 *
 * Design notes (unchanged from Lab 03):
 *  - Baseline calibration: at boot, average ACCEL_CALIB_SAMPLES samples
 *    to capture the gravity/mounting offset as a one-time snapshot, then
 *    compare every later sample's deviation from that baseline (not from
 *    zero) against the threshold. Moving the board during calibration
 *    folds that motion into the baseline -- this is a fixed snapshot, not
 *    an adaptive filter.
 *  - Send-on-change, not send-on-every-sample: an mbox notify is sent
 *    only when the motion state actually flips (STILL -> ACTIVE or back),
 *    never on every poll. A naive "send whenever over threshold" design
 *    would instead spam a notify on every single sample while the board
 *    sits just above the threshold.
 *  - The sensor stays in a low-power standby state after init until an
 *    output data rate (ODR) and full-scale range are both explicitly set
 *    with sensor_attr_set() -- skip either one and every sample silently
 *    reads back as all-zero, with no error returned.
 *  - mbox_send_dt() is called here directly from this thread's own
 *    context (main()'s own loop, never from an ISR/callback). This lab
 *    has no rx side on M4 at all (M55 never replies), so there is no
 *    mbox callback here and nothing that could call mbox_send_dt() from
 *    ISR context by mistake.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/mbox.h>

#include "ipc_common.h"

#define POLL_PERIOD_MS 100
#define ACCEL_CALIB_SAMPLES 10          /* ~1s of samples at POLL_PERIOD_MS */
#define ACCEL_THRESHOLD_MILLI_G 2000     /* deviation from baseline to trip */

static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(mc3479));

static struct mbox_dt_spec tx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

static int32_t base_x, base_y, base_z;

static int32_t abs32(int32_t v)
{
	return v < 0 ? -v : v;
}

static int read_accel_milli_g(int32_t *x, int32_t *y, int32_t *z)
{
	struct sensor_value sx, sy, sz;
	int ret = sensor_sample_fetch(accel);

	if (ret != 0) {
		return ret;
	}

	sensor_channel_get(accel, SENSOR_CHAN_ACCEL_X, &sx);
	sensor_channel_get(accel, SENSOR_CHAN_ACCEL_Y, &sy);
	sensor_channel_get(accel, SENSOR_CHAN_ACCEL_Z, &sz);

	/* struct sensor_value -> milli-g: val1 is whole g, val2 is micro-g */
	*x = sx.val1 * 1000 + sx.val2 / 1000;
	*y = sy.val1 * 1000 + sy.val2 / 1000;
	*z = sz.val1 * 1000 + sz.val2 / 1000;
	return 0;
}

static void calibrate_baseline(void)
{
	int64_t sum_x = 0, sum_y = 0, sum_z = 0;
	int got = 0;

	for (int i = 0; i < ACCEL_CALIB_SAMPLES; i++) {
		int32_t x, y, z;

		if (read_accel_milli_g(&x, &y, &z) == 0) {
			sum_x += x;
			sum_y += y;
			sum_z += z;
			got++;
		}
		k_msleep(POLL_PERIOD_MS);
	}

	if (got == 0) {
		base_x = base_y = base_z = 0;
		printk("[M4] calibration got no valid samples -- baseline left at 0,0,0\n");
		return;
	}

	base_x = (int32_t)(sum_x / got);
	base_y = (int32_t)(sum_y / got);
	base_z = (int32_t)(sum_z / got);
	printk("[M4] baseline calibrated over %d samples: x=%d y=%d z=%d milli-g\n",
	       got, base_x, base_y, base_z);
}

int main(void)
{
	printk("\n=== Lab 05 (M4): Always-On accelerometer monitor, notify M55 on state change ===\n");

	if (!device_is_ready(accel)) {
		printk("[M4] accelerometer device not ready (check DT_NODELABEL(mc3479) status)\n");
		return 0;
	}

	/* ODR (wake the sensor out of standby) -- required, see design notes. */
	struct sensor_value odr = { .val1 = 50, .val2 = 0 };

	sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);

	/* Full-scale range (fills in the driver's internal sensitivity value)
	 * -- required, see design notes. Index 0 selects the driver's
	 * default/narrowest range. */
	struct sensor_value range = { .val1 = 0, .val2 = 0 };

	sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &range);

	if (!device_is_ready(tx_channel.dev)) {
		printk("[M4] mbox tx channel not ready\n");
		return 0;
	}

	calibrate_baseline();

	enum motion_state state = MOTION_STATE_STILL;
	uint32_t seq = 0;

	printk("[M4] monitoring started (threshold=%d milli-g)\n", ACCEL_THRESHOLD_MILLI_G);

	while (1) {
		int32_t x, y, z;

		if (read_accel_milli_g(&x, &y, &z) == 0) {
			int32_t dev = abs32(x - base_x) + abs32(y - base_y) + abs32(z - base_z);
			enum motion_state new_state =
				(dev > ACCEL_THRESHOLD_MILLI_G) ? MOTION_STATE_ACTIVE
								 : MOTION_STATE_STILL;

			if (new_state != state) {
				state = new_state;
				seq++;

				struct ipc_motion_event evt = {
					.seq = seq,
					.state = (uint8_t)state,
					.deviation_mg = dev,
				};
				struct mbox_msg msg = { .data = &evt, .size = sizeof(evt) };

				printk("[M4] state changed -> %s (deviation=%d milli-g), notifying M55 (seq=%u)\n",
				       state == MOTION_STATE_ACTIVE ? "ACTIVE" : "STILL", dev, seq);
				mbox_send_dt(&tx_channel, &msg);
			}
		}

		k_msleep(POLL_PERIOD_MS);
	}

	return 0;
}

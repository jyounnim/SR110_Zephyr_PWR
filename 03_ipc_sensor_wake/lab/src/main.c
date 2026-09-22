/*
 * Lab 03 - M55 side: wait for M4's accelerometer motion-state
 * notifications over mbox and log them. No response is ever sent back to
 * M4 in this lab (a deliberately one-way "wake" channel), so M55 only
 * opens the rx side and never touches mbox_send_dt() at all.
 *
 * Design notes (hardware-confirmed 2026-09-21):
 *  - main()'s loop blocks forever on k_msgq_get(..., K_FOREVER). Every
 *    event -- ACTIVE or STILL alike -- is handled and then the loop goes
 *    straight back to that same blocking call, which is exactly where
 *    Zephyr's idle thread is free to put the CPU into WFI again. This is
 *    the same underlying mechanism as Lab 01's IDLE mode; what differs is
 *    the wake source -- not a fixed sleep timer expiring, but another
 *    core's message arriving live -- and that M55 never restarts (unlike
 *    Lab 02's watchdog reset, which restarts main() from scratch).
 *  - mbox's rx callback runs in ISR context (an M4<->M55 IPC pattern
 *    established, and hardware-confirmed, in this project's separate IPC
 *    curriculum on this same board): it must never block, so it only
 *    copies the message into a k_msgq and returns immediately. All real
 *    handling (logging here) happens in main()'s own thread instead.
 *  - Confirmed on hardware: shaking the board produces a clean
 *    STILL/ACTIVE alternation with no missed or duplicate events, and no
 *    devicetree/Kconfig mismatches turned up at build time (mbox_consumer,
 *    ipc0 and mc3479 all matched this repo's SDK as designed).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/mbox.h>
#include <string.h>

#include "ipc_common.h"

static struct mbox_dt_spec rx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);

K_MSGQ_DEFINE(evt_msgq, sizeof(struct ipc_motion_event), 8, 4);

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);

	struct ipc_motion_event evt;

	if (data->size < sizeof(evt)) {
		return;
	}

	memcpy(&evt, data->data, sizeof(evt));

	/* ISR context: never block here (no k_msleep, no mbox_send_dt) --
	 * just hand off to the queue and return. */
	k_msgq_put(&evt_msgq, &evt, K_NO_WAIT);
}

int main(void)
{
	printk("\n=== Lab 03 (M55): waiting for M4 motion-state notifications ===\n");

	if (!device_is_ready(rx_channel.dev)) {
		printk("[M55] mbox rx channel not ready\n");
		return 0;
	}

	mbox_register_callback_dt(&rx_channel, mbox_rx_callback, NULL);
	mbox_set_enabled_dt(&rx_channel, 1);

	printk("[M55] idle, waiting for events (CPU goes to WFI between them)...\n");

	while (1) {
		struct ipc_motion_event evt;

		k_msgq_get(&evt_msgq, &evt, K_FOREVER);
		printk("[M55] event seq=%u: M4 is now %s (deviation=%d milli-g)\n",
		       evt.seq, evt.state == MOTION_STATE_ACTIVE ? "ACTIVE" : "STILL",
		       evt.deviation_mg);
	}

	return 0;
}

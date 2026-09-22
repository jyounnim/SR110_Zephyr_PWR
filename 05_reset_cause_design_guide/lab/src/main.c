/*
 * Lab 05 - M55 side: this is the capstone lab. It combines two things
 * that Lab 00-04 established separately into the pattern a real product
 * would actually ship:
 *
 *  1. A boot-time reset-cause dispatch (hwinfo_get_reset_cause(), the
 *     same decode Lab 02 introduced) that classifies *why* this boot
 *     happened and logs a category-appropriate note -- a real
 *     application would use this branch point to skip/re-run
 *     initialization, raise an alarm, count reset loops, etc.
 *  2. The only wake pattern in this whole curriculum confirmed, end to
 *     end, to work exactly as intended: M4's Always-On accelerometer
 *     monitor (Lab 03) notifying M55 over mbox on a state change, with
 *     M55 sitting in WFI between events and never restarting. Reused
 *     here unchanged from Lab 03.
 *
 * What this lab deliberately does NOT reproduce, and why (see the
 * project master doc's Lab 02/Lab 04 comparison, and the summary table
 * in this lab's doc):
 *  - Lab 02's WDT_FLAG_RESET_SOC is not armed here. It was confirmed to
 *    leave the SoC in a stop state that only a physical reset clears,
 *    rather than a self-completing safety reset -- arming it in a
 *    "design guide" capstone would demonstrate a foot-gun, not a
 *    recommended pattern.
 *  - Lab 04's SW7/PMU_EN path is not exercised here. This SDK version
 *    has no software entry point into the HW low-power/ultra-low-power
 *    mode SW7 is meant to wake from (no CONFIG_PM), so there is nothing
 *    for this lab to trigger in software.
 *  - RESET_LOW_POWER_WAKE is still decoded below (in case it is ever
 *    seen coming from real RTC/PIR hardware on this board), but this lab
 *    does not attempt to cause it.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/mbox.h>
#include <string.h>

#include "ipc_common.h"

static struct mbox_dt_spec rx_channel = MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);

K_MSGQ_DEFINE(evt_msgq, sizeof(struct ipc_motion_event), 8, 4);

/*
 * Boot-time reset-cause dispatch. Same decode set as Lab 02/04
 * (RESET_PIN/RESET_SOFTWARE/RESET_WATCHDOG/RESET_POR/
 * RESET_LOW_POWER_WAKE), but instead of just printing the bitmask, each
 * category gets a one-line, product-relevant note about what a real
 * application might do about it -- this is the "standard boot handler"
 * pattern this lab exists to teach.
 */
static void handle_boot_reset_cause(void)
{
	uint32_t cause = 0;
	int ret = hwinfo_get_reset_cause(&cause);

	if (ret != 0) {
		printk("[boot] hwinfo_get_reset_cause() failed: %d\n", ret);
		return;
	}

	printk("[boot] reset cause bitmask = 0x%08x\n", cause);

	if (cause & RESET_PIN) {
		printk("[boot] -> RESET_PIN: manual/physical reset. Normal cold start.\n");
	}
	if (cause & RESET_SOFTWARE) {
		printk("[boot] -> RESET_SOFTWARE: a software-requested reset "
		       "(sys_reboot() or similar). Normal cold start.\n");
	}
	if (cause & RESET_WATCHDOG) {
		printk("[boot] -> RESET_WATCHDOG: recovering from a watchdog-triggered "
		       "reset. Lab 02 confirmed this SDK/board does not self-complete "
		       "this reset cleanly -- if you see this reliably, treat it as a "
		       "real fault signal worth logging/counting, not routine "
		       "housekeeping.\n");
	}
	if (cause & RESET_POR) {
		printk("[boot] -> RESET_POR: power-on reset. On this board this also "
		       "covers the physical System Reset button (Lab 02) and, per "
		       "Lab 04, is one of two categories a PMU_EN-driven wake "
		       "(SW7/RTC/PIR) could plausibly report -- this bit alone cannot "
		       "tell the two apart.\n");
	}
#ifdef RESET_LOW_POWER_WAKE
	if (cause & RESET_LOW_POWER_WAKE) {
		printk("[boot] -> RESET_LOW_POWER_WAKE: the AON/M4 domain woke this "
		       "boot up. This category exists in hwinfo (Lab 00) but this "
		       "curriculum found no software path to trigger it (Lab 04) -- "
		       "if this fires for real, it came from RTC/PIR/AON hardware, "
		       "not from this code.\n");
	}
#endif
	if (cause == 0) {
		printk("[boot] -> (no reset cause bits reported)\n");
	}

	/* Clear it so the NEXT reboot's cause bitmask isn't polluted by this
	 * one -- same as Lab 02/04. Note (Lab 04, hardware-confirmed): this
	 * only resets an internal software-tracked copy, not the raw
	 * AON_POR_RST hardware register itself. */
	hwinfo_clear_reset_cause();
}

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
	 * just hand off to the queue and return (Lab 03's established
	 * mbox ISR safety rule). */
	k_msgq_put(&evt_msgq, &evt, K_NO_WAIT);
}

int main(void)
{
	printk("\n=== Lab 05 (M55): reset-cause boot dispatch + Always-On sensor wake ===\n");

	handle_boot_reset_cause();

	if (!device_is_ready(rx_channel.dev)) {
		printk("[M55] mbox rx channel not ready\n");
		return 0;
	}

	mbox_register_callback_dt(&rx_channel, mbox_rx_callback, NULL);
	mbox_set_enabled_dt(&rx_channel, 1);

	printk("[M55] idle, waiting for M4 motion-state events (CPU goes to WFI between them)...\n");

	while (1) {
		struct ipc_motion_event evt;

		k_msgq_get(&evt_msgq, &evt, K_FOREVER);
		printk("[M55] event seq=%u: M4 is now %s (deviation=%d milli-g)\n",
		       evt.seq, evt.state == MOTION_STATE_ACTIVE ? "ACTIVE" : "STILL",
		       evt.deviation_mg);
	}

	return 0;
}

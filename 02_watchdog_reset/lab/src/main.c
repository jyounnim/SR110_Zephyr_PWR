/*
 * Lab 02 - M55 self watchdog (wdog0): what happens when wdt_feed() stops,
 * under each of the two reset flags this SDK's driver accepts
 * (WDT_FLAG_RESET_CPU_CORE is rejected with -EINVAL -- only per-SoC or
 * no-reset are supported at all).
 *
 * Build variant switch below selects which flag to test:
 *
 *   WDT_TEST_USE_RESET_SOC == 0  (WDT_FLAG_RESET_NONE)
 *     The callback keeps firing again every WDT_TIMEOUT_MS, indefinitely,
 *     with no hang and no reset -- a repeating notifier, not a safety net.
 *
 *   WDT_TEST_USE_RESET_SOC == 1  (WDT_FLAG_RESET_SOC)
 *     The callback fires once, then the WHOLE SoC (M55 AND M4) halts and
 *     does not self-recover -- only a physical reset button press brings
 *     it back.
 *
 * Design notes (hardware-confirmed 2026-09-21; see the troubleshooting
 * doc for the full investigation):
 *  - Both behaviors above contradict what Lab 00's driver source review
 *    had predicted (troubleshooting doc issues 1 and 2).
 *  - RESET_SOC halting M4 as well as M55 also contradicts the repeated
 *    claim, since Lab 00, that M4 is Always-On and unaffected by M55's
 *    state -- that claim does not hold for this reset path.
 *  - Whether WDT_FLAG_RESET_SOC's reset request actually reaches the
 *    hardware is left unresolved: this board's physical reset button
 *    always reports RESET_POR regardless of what preceded it (confirmed
 *    with a baseline test unrelated to this lab), so
 *    hwinfo_get_reset_cause() cannot be used here to tell "the reset was
 *    latched but got stuck" apart from "the reset request had no effect
 *    at all". Getting further would need a debugger probing the
 *    watchdog's registers directly -- out of scope for this lab.
 *
 * Change the switch below and reflash to run the other variant. Because
 * RESET_SOC's callback fires but the board then needs a physical reset to
 * recover, budget for a manual reset-button press whenever you run the
 * WDT_TEST_USE_RESET_SOC == 1 build.
 */

#define WDT_TEST_USE_RESET_SOC 1

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/hwinfo.h>

/* Timeout window for the single installed channel. With RESET_NONE the
 * callback keeps repeating every WDT_TIMEOUT_MS; with RESET_SOC the
 * callback fires once around this time after the last feed, and the SoC
 * halts shortly after (see the file header above). */
#define WDT_TIMEOUT_MS 2000

/* Feed the watchdog this many times, once per second, before deliberately
 * stopping -- this proves feeding does prevent the timeout from firing. */
#define WDT_FEED_COUNT 5
#define WDT_FEED_PERIOD_MS 1000

/* After the last feed, print a heartbeat this often so the console log
 * timestamps can be used to measure how long the reset actually took. */
#define POST_FEED_HEARTBEAT_MS 250

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdog0));

static volatile bool callback_fired;
static volatile uint32_t callback_count;

static void wdt_callback(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);

	/* This runs in the watchdog driver's own callback context (not
	 * main()). Keep it short -- do not call wdt_feed() here, since the
	 * whole point of this lab is to observe what happens when the
	 * timeout is NOT fed in time.
	 *
	 * NOTE: with WDT_FLAG_RESET_NONE, hardware testing showed this
	 * callback keeps firing again every WDT_TIMEOUT_MS forever -- it is
	 * NOT a one-shot event on this SDK. callback_count makes that
	 * repetition visible in the log. */
	callback_fired = true;
	callback_count++;
	printk(">> [WDT] callback fired on channel %d (call #%u) -- NOT feeding, watching what happens next\n",
	       channel_id, callback_count);

#if WDT_TEST_USE_RESET_SOC
	printk(">> [WDT] RESET_SOC mode: expect the WHOLE SoC (M55 AND M4) to halt shortly --\n");
	printk(">> [WDT] this does not self-recover; a physical reset button press is required (see troubleshooting doc, issue 2)\n");
#else
	printk(">> [WDT] RESET_NONE mode: no SoC reset will occur -- expect this callback to keep repeating every %d ms indefinitely (see troubleshooting doc, issue 1)\n",
	       WDT_TIMEOUT_MS);
#endif
}

/* Decode and print hwinfo's reset-cause bitmask in human-readable form.
 * Called once at the very start of main(), before doing anything else,
 * so a watchdog-triggered reboot is identified before any other code runs. */
static void print_and_clear_reset_cause(void)
{
	uint32_t cause = 0;
	int ret = hwinfo_get_reset_cause(&cause);

	if (ret != 0) {
		printk("[boot] hwinfo_get_reset_cause() failed: %d (TBD -- CONFIG_HWINFO issue?)\n",
		       ret);
		return;
	}

	printk("[boot] reset cause bitmask = 0x%08x ->", cause);
	if (cause & RESET_PIN) {
		printk(" RESET_PIN");
	}
	if (cause & RESET_SOFTWARE) {
		printk(" RESET_SOFTWARE");
	}
	if (cause & RESET_WATCHDOG) {
		printk(" RESET_WATCHDOG");
	}
	if (cause & RESET_POR) {
		printk(" RESET_POR");
	}
#ifdef RESET_LOW_POWER_WAKE
	if (cause & RESET_LOW_POWER_WAKE) {
		printk(" RESET_LOW_POWER_WAKE");
	}
#endif
	if (cause == 0) {
		printk(" (none reported)");
	}
	printk("\n");

	if (cause & RESET_WATCHDOG) {
		printk(">> [boot] This boot was caused by the M55 watchdog -- Lab 02's SOC reset just happened.\n");
	}

	/* Clear it so the NEXT reboot's cause bitmask isn't polluted by
	 * this one. */
	hwinfo_clear_reset_cause();
}

int main(void)
{
	/* Step 0: always check (and clear) the reset cause first, before
	 * anything else -- this is the standard pattern later labs will
	 * reuse (see Lab 05). */
	print_and_clear_reset_cause();

	/* Step 0b: a plain (non-retained) global/local counter always
	 * starts over at its initial value on every boot, whether that
	 * boot followed a watchdog reset or a normal power-on. This lab
	 * does not add any battery-backed/retained storage, so there is
	 * no way for a counter to "remember" previous boots -- printing
	 * this value each boot is itself the proof that a watchdog reset
	 * is a full cold reboot, not a resume (contrast with Lab 01's WFI,
	 * which never restarts main() at all).
	 */
	static uint32_t boot_local_counter = 12345; /* deliberately nonzero */

	printk("\n=== Lab 02: M55 Watchdog (wdog0) Reset Test ===\n");
	printk("Build variant: WDT_TEST_USE_RESET_SOC=%d (%s)\n", WDT_TEST_USE_RESET_SOC,
	       WDT_TEST_USE_RESET_SOC ? "WDT_FLAG_RESET_SOC" : "WDT_FLAG_RESET_NONE");
	printk("[boot] boot_local_counter = %u (always reinitializes to 12345 on a cold boot --\n",
	       boot_local_counter);
	printk("        if this were ever anything else, it would mean the reset was NOT a full reboot)\n");

	if (!device_is_ready(wdt)) {
		printk("wdog0 device not ready -- check the board overlay (&wdog0 status)\n");
		return 0;
	}

	struct wdt_timeout_cfg wdt_cfg = {
		.window = {.min = 0, .max = WDT_TIMEOUT_MS},
		.callback = wdt_callback,
		.flags = WDT_TEST_USE_RESET_SOC ? WDT_FLAG_RESET_SOC : WDT_FLAG_RESET_NONE,
	};

	int channel_id = wdt_install_timeout(wdt, &wdt_cfg);

	if (channel_id < 0) {
		printk("wdt_install_timeout() failed: %d\n", channel_id);
		return 0;
	}

	int ret = wdt_setup(wdt, 0);

	if (ret != 0) {
		printk("wdt_setup() failed: %d\n", ret);
		return 0;
	}

	printk("[main] watchdog armed: timeout=%d ms, channel=%d\n", WDT_TIMEOUT_MS, channel_id);

	/* Feed on schedule for a while first, to prove that feeding really
	 * does prevent the timeout from firing. */
	for (int i = 1; i <= WDT_FEED_COUNT; i++) {
		k_msleep(WDT_FEED_PERIOD_MS);
		wdt_feed(wdt, channel_id);
		printk("[main] fed watchdog (%d/%d), uptime=%u ms\n", i, WDT_FEED_COUNT,
		       k_uptime_get_32());
	}

	/* Now deliberately stop feeding and watch what happens. */
	uint32_t stop_feed_uptime = k_uptime_get_32();

	printk("[main] STOPPING FEED NOW at uptime=%u ms -- watching for timeout/reset\n",
	       stop_feed_uptime);

	while (1) {
		printk("[main] still running, %u ms since last feed (callback_fired=%d, callback_count=%u)\n",
		       k_uptime_get_32() - stop_feed_uptime, callback_fired, callback_count);
		k_msleep(POST_FEED_HEARTBEAT_MS);
	}

	return 0;
}

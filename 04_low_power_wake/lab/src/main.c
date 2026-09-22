/*
 * Lab 04 - M55 side: the most exploratory lab in this curriculum, and the
 * one with the narrowest confirmed scope. There is no application-level
 * API to trigger or observe the board's physical "Wake-up" button
 * (schematic-identified as SW7) or the RESET_LOW_POWER_WAKE path it may
 * lead to -- see Lab 00's SDK/schematic review and the project master doc.
 * This lab does not try to synthesize that trigger in software; it only
 * watches for it.
 *
 * What this code does:
 *  1. Dumps the raw AON_POR_RST register (0x50350038) read-only, both
 *     before and right after hwinfo_clear_reset_cause() -- the same
 *     shared register hwinfo_get_reset_cause() decodes (M4 = bit8,
 *     M55 = bit9, Lab 00 SDK review), so any undocumented bits are
 *     visible and the clear call's real effect on hardware (as opposed
 *     to an internal software cache) can be checked directly.
 *  2. Decodes/clears the reset cause with the same pattern established in
 *     Lab 02 (print_and_clear_reset_cause()), plus the same
 *     boot_local_counter cold-boot marker.
 *  3. Runs a heartbeat loop that also live-polls the same raw register
 *     every tick (2 s), so a press of SW7 can be checked for a silent bit
 *     change without needing a reboot to see it.
 *
 * Design notes (hardware-confirmed 2026-09-21):
 *  - Pressing SW7 while the board is already running produces NO visible
 *    effect through any software-observable channel: no reboot on either
 *    core, no reset-cause change, and -- confirmed via ~26 s / 13 ticks of
 *    continuous live polling while repeatedly pressing SW7 -- not a
 *    single bit change in the raw AON_POR_RST register either.
 *  - hwinfo_clear_reset_cause() does NOT clear this raw register: the
 *    post-clear dump matched the pre-clear dump exactly (0x00000fff both
 *    times). The driver evidently only resets an internal software-
 *    tracked copy; the hardware register itself stays latched until an
 *    actual POR.
 *  - Why this is the expected result, not a bug: if SW7 really drives
 *    SR110_PMU_EN (per the board's own "PMU_EN to wake-up SR110"
 *    labeling), it is a signal meant to (re-)enable a PMU that is OFF or
 *    in a HW-level low-power/ultra-low-power state -- re-asserting an
 *    enable signal on an already-enabled PMU has nothing to change.
 *    Testing SW7's *meaningful* wake-up semantics would require first
 *    commanding SR110 into that HW power state from software, and this
 *    SDK has no CONFIG_PM, no PM_STATE_*, no such entry API at all
 *    (Lab 00 SDK review, master doc section 1-2 item 1). A follow-up test
 *    of fully power-cycling the board and pressing SW7 would only confirm
 *    the PMU_EN wiring (a cold power-on path), not a genuine software-
 *    commanded low-power-mode wake -- so it is left as an optional,
 *    out-of-scope experiment rather than a requirement for this lab.
 *  - Net conclusion: this SDK/board combination has no software-
 *    reproducible path to trigger or observe RESET_LOW_POWER_WAKE. Its
 *    existence as a hwinfo category, and the SW7/RTC/PIR -> EXT_INT ->
 *    PMU_EN path seen in the schematic, are confirmed; driving that path
 *    from software is not possible with this SDK version.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/hwinfo.h>

#define HEARTBEAT_PERIOD_MS 2000

/* Same shared reset-cause register hwinfo_get_reset_cause() decodes
 * (Lab 00 SDK review: AON_POR_RST @ 0x50350038, M4 = bit8, M55 = bit9).
 * Read-only here -- this dump exists purely to surface any bits Zephyr's
 * hwinfo API does not name, in case the SW7/RTC/PIR trigger path sets
 * something outside RESET_PIN/RESET_SOFTWARE/RESET_WATCHDOG/RESET_POR/
 * RESET_LOW_POWER_WAKE. */
#define AON_POR_RST_ADDR 0x50350038

static void dump_raw_aon_por_rst(const char *label)
{
	uint32_t raw = sys_read32(AON_POR_RST_ADDR);

	printk("[boot] raw AON_POR_RST (0x%08x) = 0x%08x (read-only, %s)\n",
	       AON_POR_RST_ADDR, raw, label);
}

/* Same decode/clear pattern established in Lab 02 -- see that lab's code
 * for the full category rationale (RESET_PIN/RESET_SOFTWARE/
 * RESET_WATCHDOG/RESET_POR/RESET_LOW_POWER_WAKE). */
static void print_and_clear_reset_cause(void)
{
	uint32_t cause = 0;
	int ret = hwinfo_get_reset_cause(&cause);

	if (ret != 0) {
		printk("[boot] hwinfo_get_reset_cause() failed: %d\n", ret);
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

	/* Clear it so the NEXT reboot's cause bitmask isn't polluted by
	 * this one. */
	hwinfo_clear_reset_cause();
}

int main(void)
{
	uint32_t tick = 0;

	/* Order matters: dump the raw register BEFORE clearing the decoded
	 * cause, so the raw dump reflects the same boot the decoded cause
	 * describes. */
	dump_raw_aon_por_rst("pre-clear");
	print_and_clear_reset_cause();

	/* Read it again right after the clear call, to see whether
	 * hwinfo_clear_reset_cause() actually writes to this hardware
	 * register or only resets an internal software-tracked copy (a
	 * common pattern for reset-cause drivers). If this still reads the
	 * same as the pre-clear dump, the raw register itself was never
	 * touched by the clear call. */
	dump_raw_aon_por_rst("post-clear");

	/* Cold-boot marker, same pattern as Lab 02: this always reinitializes
	 * to 12345 on any full reboot, whatever triggered it. */
	static uint32_t boot_local_counter = 12345;

	printk("\n=== Lab 04 (M55): Low-Power-Wake / SW7 exploration ===\n");
	printk("[boot] boot_local_counter = %u (always 12345 on a cold boot)\n",
	       boot_local_counter);
	printk("[main] running normally now -- try the physical Wake-up button (SW7)\n");
	printk("[main] and watch this console (and the raw register dump on any reboot)\n");

	while (1) {
		/* Live poll, every tick: this is what lets a press of SW7 be
		 * checked for a bit change WITHOUT needing a reboot to see it
		 * (the earlier boot-only dump above could only ever show a
		 * change across a reboot). Read-only, cheap, and safe to do
		 * every 2 seconds indefinitely. */
		uint32_t raw = sys_read32(AON_POR_RST_ADDR);

		printk("[main] heartbeat tick=%u, uptime=%u ms, raw AON_POR_RST=0x%08x\n",
		       tick++, k_uptime_get_32(), raw);
		k_msleep(HEARTBEAT_PERIOD_MS);
	}

	return 0;
}

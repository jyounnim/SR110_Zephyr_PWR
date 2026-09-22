/*
 * Lab 04 - M4 side: fixed heartbeat, unchanged in spirit from Lab 01/02.
 *
 * Kept running here so its own tick counter stays visible in the console
 * alongside M55's Low-Power-Wake/SW7 experiment. If pressing SW7 (or
 * whatever trigger is under test) turns out to reset the whole SoC -- the
 * way Lab 02's WDT_FLAG_RESET_SOC did -- this heartbeat halting at the same
 * moment as M55's is the evidence for that; if it keeps ticking right
 * through whatever happens to M55, that is evidence the trigger stayed
 * local to M55 instead.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define HEARTBEAT_PERIOD_MS 1000

int main(void)
{
	uint32_t tick = 0;

	printk("\n=== Lab 04 (M4): heartbeat (halts only if the trigger under test resets the whole SoC) ===\n");

	while (1) {
		printk("[M4 heartbeat] tick=%u, uptime=%u ms\n", tick++, k_uptime_get_32());
		k_msleep(HEARTBEAT_PERIOD_MS);
	}

	return 0;
}

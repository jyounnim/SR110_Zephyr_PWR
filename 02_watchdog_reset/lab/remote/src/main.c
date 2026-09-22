/*
 * Lab 02 - M4 side: fixed heartbeat, unchanged from Lab 01.
 *
 * Kept running here unchanged so its tick counter is directly visible in
 * the console alongside M55's watchdog test. Design note (hardware-
 * confirmed 2026-09-21, see the Lab 02 troubleshooting doc issue 2 for
 * the full investigation): this heartbeat keeps ticking through M55's
 * IDLE/WFI (Lab 00/01), but halts at the exact same moment M55 does when
 * M55's watchdog fires with WDT_FLAG_RESET_SOC and is not fed -- i.e. M4
 * being "Always-On" elsewhere in this curriculum does NOT make it immune
 * to M55's watchdog SoC reset. Do not assume M4 keeps running through
 * every M55 state change without checking which kind it is.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define HEARTBEAT_PERIOD_MS 1000

int main(void)
{
	uint32_t tick = 0;

	printk("\n=== Lab 02 (M4): heartbeat (halts if M55's watchdog fires with RESET_SOC) ===\n");

	while (1) {
		printk("[M4 heartbeat] tick=%u, uptime=%u ms\n",
		       tick++, k_uptime_get_32());
		k_msleep(HEARTBEAT_PERIOD_MS);
	}

	return 0;
}

/*
 * Lab 01 - M4 side: fixed Always-On workload.
 *
 * M4 is the Always-On core in this SoC and its power state never changes
 * in this curriculum -- only M55's does. This lab is about isolating
 * M55's own Active/Idle current delta, so M4 must contribute the *same*
 * baseline current regardless of which mode M55 is in. A simple, fixed
 * 1-second heartbeat satisfies that without requiring any external
 * sensor hardware to be wired up yet.
 *
 * This heartbeat is a placeholder for M4's real Always-On monitoring
 * role (e.g. periodic sensor polling). A real sensor-based workload is
 * introduced once IPC between M4 and M55 is added in a later lab.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define HEARTBEAT_PERIOD_MS 1000

int main(void)
{
	uint32_t tick = 0;

	printk("\n=== Lab 01 (M4): Always-On heartbeat ===\n");

	while (1) {
		printk("[M4 heartbeat] tick=%u, uptime=%u ms\n",
		       tick++, k_uptime_get_32());
		k_msleep(HEARTBEAT_PERIOD_MS);
	}

	return 0;
}

/*
 * Lab 01 - Baseline: M55 Active vs Idle (WFI) current measurement
 *
 * Lab 00's SDK review confirmed this SDK (syna_zephyr_sdk-1.0.0) does not
 * implement Zephyr's CONFIG_PM subsystem. The only power-saving mechanism
 * available on M55 is the kernel's own idle thread, which automatically
 * executes WFI whenever no application thread is ready to run. This lab
 * adds no new power-management code -- it just gives M55 two selectable
 * modes so this existing behavior can be measured with a multimeter:
 *
 *   ACTIVE - a loop that never blocks, so it is always ready to run.
 *            The idle thread (and WFI) is therefore never scheduled.
 *   IDLE   - a loop that calls k_msleep() between short log bursts, so
 *            the CPU has nothing to do in between, and the idle thread
 *            runs WFI repeatedly during that gap.
 *
 * Boots into IDLE by default. Press the onboard SW8 ("USER_BUTTON") to
 * toggle between ACTIVE and IDLE.
 *
 * Design notes (see the troubleshooting doc for the full investigation
 * history -- several earlier approaches were tried and ruled out):
 *
 * - SW8 is read as a plain GPIO (gpio_pin_get_dt()), not through Zephyr's
 *   gpio-keys/input subsystem. That subsystem's driver in this SDK has a
 *   confirmed bug: both its interrupt mode and its polling-mode stop
 *   delivering events after the very first press. Reading the raw GPIO
 *   directly bypasses this entirely (troubleshooting doc, issues 4-6).
 *
 * - The button is polled from inside main()'s own loop, not a separate
 *   thread, with different timing per mode so neither mode's current
 *   profile is disturbed by the poll itself:
 *     - IDLE:   sampled once, right after main() wakes from its own
 *               k_msleep(IDLE_SLEEP_MS). This adds zero extra wake
 *               events, so the measured IDLE current stays clean.
 *     - ACTIVE: sampled every ACTIVE_BUTTON_POLL_PERIOD_MS via a
 *               non-blocking k_uptime_get_32() comparison (never
 *               k_msleep()), so ACTIVE never gains an idle gap of
 *               its own.
 *   An earlier revision used a dedicated always-running polling thread,
 *   which woke the CPU every 20 ms regardless of mode and silently
 *   contaminated the IDLE current measurement (troubleshooting doc,
 *   issue 7).
 *
 * - Confirmed on hardware (2026-09-21): SW8 toggles ACTIVE/IDLE reliably
 *   across repeated presses. IDLE->ACTIVE has an inherent ~1-2 s response
 *   delay (a direct result of the once-per-2s IDLE sampling above);
 *   ACTIVE->IDLE is near-instant (20 ms polling). This latency/power
 *   trade-off is intentional, not a bug -- see the troubleshooting doc.
 *
 * Hardware note: SW8 is wired through the PCA6416A I2C GPIO expander
 * (gpio_exp0) on the shared I2C1 bus, not to a direct SoC GPIO pin. This
 * lab's board overlays have M55 enable i2c1/gpio_exp0 and M4 disable
 * them, so only one core masters the shared bus at a time (the same
 * "one physical I2C1 bus, only one master" rule from the M4<->M55 IPC
 * curriculum).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>

#define MODE_IDLE 0
#define MODE_ACTIVE 1

#define IDLE_SLEEP_MS 2000
#define ACTIVE_LOG_PERIOD_MS 2000

/* Fast polling only applies in ACTIVE mode, where the CPU is already
 * busy and never sleeps -- see the v4 redesign note above. */
#define ACTIVE_BUTTON_POLL_PERIOD_MS 20
#define ACTIVE_BUTTON_DEBOUNCE_SAMPLES 3

/* SW8 ("USER_BUTTON"), read directly as a plain GPIO -- see the v3
 * redesign note above for why this bypasses Zephyr's gpio-keys driver.
 * This reuses the `gpios` property already defined on the existing
 * `user_button` devicetree node (the same node the M4<->M55 IPC
 * curriculum uses), so no devicetree changes were needed for this file. */
static const struct gpio_dt_spec sw8 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_button), gpios);

static bool sw8_ready;

static void sw8_init(void)
{
	if (!gpio_is_ready_dt(&sw8))
	{
		printk("SW8 GPIO device not ready -- button disabled\n");
		return;
	}

	if (gpio_pin_configure_dt(&sw8, GPIO_INPUT) != 0)
	{
		printk("Failed to configure SW8 as GPIO input -- button disabled\n");
		return;
	}

	sw8_ready = true;
}

static void toggle_mode(int *mode)
{
	if (*mode == MODE_ACTIVE)
	{
		*mode = MODE_IDLE;
		printk(">> [SW8] IDLE mode: CPU sleeps via k_msleep()/WFI between bursts\n");
	}
	else
	{
		*mode = MODE_ACTIVE;
		printk(">> [SW8] ACTIVE mode: CPU stays ready, WFI never runs\n");
	}
}

int main(void)
{
	uint32_t now;
	uint32_t last_log_ms = 0;
	uint32_t last_btn_poll_ms = 0;
	int mode = MODE_IDLE;

	/* ACTIVE-mode debounce state (fast sample-and-hold, reused from v3). */
	int active_candidate = 0;
	int active_candidate_count = 0;
	int active_stable_state = 0;

	/* IDLE-mode state: last sample taken right after waking from
	 * k_msleep(IDLE_SLEEP_MS). No debounce needed -- see the v4
	 * redesign note above. */
	int idle_last_state = 0;

	int val;

	sw8_init();

	printk("\n=== Lab 01: M55 Active vs Idle (WFI) Baseline ===\n");
	printk("Press the onboard SW8 (USER_BUTTON) to toggle ACTIVE/IDLE mode\n");
	printk("Boot mode: IDLE\n");

	while (1)
	{
		if (mode == MODE_ACTIVE)
		{
			/*
			 * Never call a blocking API in this branch. As long as
			 * this thread stays ready-to-run, the idle thread (and
			 * its automatic WFI) is never scheduled.
			 */
			k_yield();

			now = k_uptime_get_32();

			/*
			 * Non-blocking button poll: gated by elapsed time, not
			 * k_msleep(), so this branch never sleeps and ACTIVE's
			 * current profile stays undisturbed.
			 */
			if (sw8_ready && (now - last_btn_poll_ms) >= ACTIVE_BUTTON_POLL_PERIOD_MS)
			{
				last_btn_poll_ms = now;
				val = gpio_pin_get_dt(&sw8);

				if (val >= 0)
				{
					if (val == active_candidate)
					{
						active_candidate_count++;
					}
					else
					{
						active_candidate = val;
						active_candidate_count = 1;
					}

					if (active_candidate_count == ACTIVE_BUTTON_DEBOUNCE_SAMPLES &&
						active_candidate != active_stable_state)
					{
						active_stable_state = active_candidate;

						if (active_stable_state == 1)
						{
							toggle_mode(&mode);
							idle_last_state = active_stable_state;
							continue; /* re-enter the loop and take the IDLE branch immediately */
						}
					}
				}
			}

			if ((now - last_log_ms) >= ACTIVE_LOG_PERIOD_MS)
			{
				printk("[ACTIVE] uptime=%u ms -- CPU never idles\n", now);
				last_log_ms = now;
			}
		}
		else
		{
			now = k_uptime_get_32();
			printk("[IDLE] uptime=%u ms -- sleeping %d ms (WFI expected)\n",
				   now, IDLE_SLEEP_MS);
			k_msleep(IDLE_SLEEP_MS);

			/*
			 * Single sample, taken right after the CPU already woke up
			 * for this iteration's own log line -- this adds no extra
			 * wake event beyond what IDLE mode already does. See the
			 * v4 redesign note above for why no debounce is needed here.
			 */
			if (sw8_ready)
			{
				val = gpio_pin_get_dt(&sw8);

				if (val == 1 && idle_last_state == 0)
				{
					toggle_mode(&mode);

					/* Reset ACTIVE's fast-debounce state so a fresh
					 * press-cycle starts clean the next time ACTIVE
					 * is entered. */
					active_candidate = 0;
					active_candidate_count = 0;
					active_stable_state = 1;
				}

				if (val >= 0)
				{
					idle_last_state = val;
				}
			}
		}
	}

	return 0;
}

# Lab 01 — Baseline: M55 Active vs Idle (WFI) Current Measurement

## Learning Objectives

- Understand the Zephyr kernel's idle thread, which automatically enters WFI (Wait For Interrupt) regardless of the `CONFIG_PM` setting.
- Directly measure how much difference there is, in actual CPU power draw, between an application thread staying "always ready to run" versus "waiting on a blocking call."
- Practice toggling M55 between ACTIVE and IDLE modes in real time via an onboard button, comparing the current of both states from a single firmware image without reflashing.
- Establish this measurement as the current/latency baseline for comparison in later labs (watchdog reset, IPC wake, PMU_EN-based wake).

## Connection to Lab 00

Lab 00 (SDK and schematic source review) confirmed the following key facts:

- This SDK (`syna_zephyr_sdk-1.0.0`) does not implement Zephyr's standard Power Management subsystem (`CONFIG_PM`, `enum pm_state`) at all.
- Therefore, the only low-power mechanism actually usable by an application on this SDK is the automatic WFI entry of the Zephyr kernel's default idle thread. This runs automatically, with no extra configuration, whenever "no thread is ready to run."

This lab is the first hands-on lab that verifies this fact by direct measurement. The goal is not to add any new low-power API, but to **observe the already-existing idle thread's behavior as it switches on and off.**

## Core Concept: Zephyr Idle Thread and WFI

The moment there is no ready application thread, the Zephyr scheduler automatically runs the lowest-priority idle thread. On ARM Cortex-M, this idle thread calls `k_cpu_idle()` (internally the CMSIS `__WFI()` instruction), which gates the CPU clock and waits until the next interrupt occurs. Registers and RAM contents are fully preserved, and execution resumes immediately at the instruction after WFI once an interrupt fires — as summarized in Lab 00, this is "the shallowest sleep state, with no loss of context."

The difference between the two modes in this lab:

| Mode | Thread behavior | Does the idle thread run? | Expected current |
|---|---|---|---|
| **ACTIVE** | Repeats `k_yield()` with no blocking call — the thread stays ready at all times | Never runs (no WFI) | Relatively high |
| **IDLE** | Waits 2 seconds at a time via `k_msleep(2000)` — no ready thread in between | Runs continuously during the wait (WFI repeatedly) | Relatively low |

`k_yield()` hands the CPU to another ready thread of the same priority if one exists; if not, the calling thread simply keeps running immediately — **it is not a function that yields to the idle thread.** So in ACTIVE mode, the idle thread never gets a chance to be scheduled at all.

## Hardware/Measurement Setup

- An SR110 RDK board and a multimeter capable of current measurement (ideally one that displays down to mA/µA resolution).
- This lab uses no LEDs or sensors whatsoever — they are deliberately excluded so the measurement reflects only the CPU's Active/Idle state. That said, per the design below, the I2C1 GPIO expander needed to read the onboard button (SW8) is enabled on M55 (its impact on current is expected to be minimal, but this is TBD).
- **TBD**: This review did not confirm whether the board has a dedicated measurement point (e.g., a shunt resistor or jumper on the power tree) that isolates just the CPU/SoC current. If no such point exists, connect the multimeter in series on the board's overall input power line (e.g., the battery/USB supply) to measure total current, and interpret the **difference** seen when toggling ACTIVE↔IDLE as attributable to M55, on the basis that M4's heartbeat workload is fixed (see "Design Note" below).

## Design Note — Why M4 Only Runs a Fixed Heartbeat

On this SoC, M4 is Always-On and only M55's power state changes. Since this lab's purpose is to measure "the difference in M55's Active/Idle current," M4's power consumption must stay **exactly identical** across the two measurements (ACTIVE vs IDLE) so that only the difference caused by M55 can be compared cleanly. That's why M4 is fixed to a simple workload that only prints a heartbeat log once per second. A real sensor-based Always-On watch (e.g., polling a temperature sensor) is not yet wired into this lab — it is swapped in as the real workload in Lab 03, which covers M4↔M55 IPC.

## Hardware Configuration

- **M55 console**: connect via either the board's USB-C connector (through the onboard bridge, 115200bps) or the header for an external UART-USB converter (230400bps, 8N1) — whichever is convenient. Both paths are physically the same M55 UART1 signal lines. This console is output-only (for viewing logs).
- **M4 console**: connect via a separate USB-UART adapter. The exact physical connector designation was not confirmed in this review (**TBD**) — use the same connection method you used in the earlier M4↔M55 IPC labs.
- **Mode switching**: press the onboard push button **SW8 ("USER_BUTTON")**. No extra wiring is required. This lab never uses the console UART's RX path at all.

## Devicetree Overlays

This lab uses I2C1 on M55 (and the GPIO expander `gpio_exp0` hanging off it) to read the onboard button SW8. Since I2C1 is physically a single bus shared by M4/M55 (a fact confirmed on hardware in the separate M4↔M55 IPC lab series), the two cores conflict if both try to initialize this bus as master at the same time. Since M4 does not use I2C1 in this lab, **only M55 enables I2C1, and M4 disables it**.

`lab/boards/sr100_rdk_sr100_m55.overlay`:
```dts
/* &i2c1 / &gpio_exp0 are left at their base devicetree default ("okay")
 * by not overriding them here -- SW8 is read through gpio_exp0. */
&ov02c10 {
	status = "disabled";
};
```

`lab/remote/boards/sr100_rdk_sr100_m4.overlay`:
```dts
&i2c1 {
	status = "disabled";
};

&gpio_exp0 {
	status = "disabled";
};
```

The `ov02c10` (camera sensor) node is disabled only on the M55 side — whether the same node exists in the M4 devicetree was not confirmed (**TBD**), and referencing a node that doesn't exist in an overlay causes a devicetree compile error, so it was left out of the M4 side.

## Code Structure

### M55 (`lab/src/main.c`)

This lab does not use Zephyr's `gpio-keys`/input subsystem — SW8 ("USER_BUTTON") is fetched directly from the `gpios` property of the `user_button` devicetree node (`GPIO_DT_SPEC_GET()`) and read with the plain GPIO API (`gpio_pin_get_dt()`). (This SDK's `gpio-keys` driver has a real bug that this works around — see `01_baseline_active_idle_troubleshooting_en.md` for details.)

Button polling is not done from a separate thread — it is **folded into `main()`'s existing loop, with different behavior per mode**. A dedicated always-running polling thread would wake up periodically even in IDLE mode, preventing the CPU from ever truly sleeping via WFI and contaminating the very IDLE current this lab is trying to measure.

- **IDLE mode**: the button is read exactly once, right after the `k_msleep(2000)` call that's already being made for the status log. Since the CPU was going to wake up at that point anyway, this poll adds zero extra wake events to IDLE's current profile. A single sample is sufficient, with no separate debounce logic, because the 2-second interval itself is far longer than mechanical button chatter (a few ms).
- **ACTIVE mode**: the same sample-and-debounce logic is used — a state change is only accepted after 3 consecutive identical readings at a 20 ms period — but that period is implemented not with `k_msleep()` but the same way as the existing `ACTIVE_LOG_PERIOD_MS` log (a non-blocking comparison of `k_uptime_get_32()` values). Since ACTIVE mode's entire reason for existing is "the CPU stays ready at all times, so WFI never runs," calling `k_msleep()` anywhere in this branch would itself reintroduce an idle gap.
- The mode is only toggled on the press edge (the moment the state settles from 0→1) — this treats one press+release as a single toggle.
- Since there is now only one thread (`main()` itself), there is nothing that can be starved by a priority issue.

**Hardware verification complete (2026-09-21)**: after pressing SW8 repeatedly many times, ACTIVE↔IDLE toggling kept working correctly.

**Known characteristic (not a bug)**: because IDLE mode samples the button only once every 2 seconds, there is a delay of up to about 2 seconds (roughly 1 second on average) between pressing SW8 and actually transitioning from IDLE to ACTIVE. The ACTIVE→IDLE direction responds essentially instantly, since it polls every 20 ms. This asymmetry is an intentional trade-off — responding faster in IDLE would require waking up more often, which would contaminate the very IDLE current this lab is trying to measure. **This is a good place to directly feel the trade-off between "low power" and "fast responsiveness,"** so this lab leaves it as-is rather than improving it (a lower-latency option would be to configure a raw GPIO interrupt directly, but this curriculum does not pursue that).

### M4 (`lab/remote/src/main.c`)

All it does is print `[M4 heartbeat] tick=N, uptime=... ms` once per second. It behaves identically regardless of M55's mode, so only M55's Active/Idle current difference can be compared in isolation.

## prj.conf

`lab/prj.conf` (M55) — in addition to console output settings, only `CONFIG_GPIO`/`CONFIG_I2C` (via the I2C expander) are needed to read SW8 as a plain GPIO. Since Zephyr's input subsystem (`gpio-keys`) is not used, `CONFIG_INPUT`/`CONFIG_INPUT_GPIO_KEYS` are not required:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_GPIO=y
CONFIG_I2C=y
```

`lab/remote/prj.conf` (M4) — uses only the minimal settings needed for console output:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y
```

## Build Instructions

On this SoC, M4 is built first, and its output (the M4 ELF) is packaged into the M55 build to produce a single combined flash image (the same approach used in the separate M4↔M55 IPC lab series). From the workspace root:

```bash
# 1) Build M4 first.
west build -p always -b sr100_rdk/sr100/m4 -d m4 01_baseline_active_idle/lab/remote

# 2) Build M55, packaging in the M4 image just built.
#    M4_BUILD is a relative path from M55's build directory (the path given with -d).
#    The example below assumes the two build directories (m4/, m55/) sit side by side.
west build -p always -b sr100_rdk/sr100/m55 -d m55 01_baseline_active_idle/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

**Note**: after changing an overlay/devicetree, make sure you're using `-p always`. Reusing an existing build directory can hit a build-cache issue where the change isn't picked up.

Flashing is done via the OpenOCD-based script (`srsdk_tools/openocd_flash.py`). The exact arguments (toolchain/config file paths) depend on your local environment setup, so use whatever method you've been using.

## Running and Checking Results

1. Open a serial terminal for each of the M55 and M4 consoles.
2. After flashing and rebooting, the M55 console boots into IDLE mode with a banner like this:
   ```
   === Lab 01: M55 Active vs Idle (WFI) Baseline ===
   Press the onboard SW8 (USER_BUTTON) to toggle ACTIVE/IDLE mode
   Boot mode: IDLE
   [IDLE] uptime=12 ms -- sleeping 2000 ms (WFI expected)
   [IDLE] uptime=2013 ms -- sleeping 2000 ms (WFI expected)
   ...
   ```
   The M4 console independently prints `[M4 heartbeat] tick=N, uptime=... ms` once per second.
3. With the board in this state (IDLE), measure the current with the multimeter and record the value.
4. Press the onboard **SW8** button once to switch to ACTIVE mode. **By design, the IDLE→ACTIVE transition has a delay of up to about 2 seconds (roughly 1 second on average), so it's normal for nothing to happen the instant you press it** — wait a moment (see "Known characteristic" under "Code Structure" above).
   ```
   >> [SW8] ACTIVE mode: CPU stays ready, WFI never runs
   [ACTIVE] uptime=4021 ms -- CPU never idles
   [ACTIVE] uptime=6021 ms -- CPU never idles
   ...
   ```
5. Wait a few seconds for it to settle, then measure and record the current the same way.
6. Press SW8 once more to return to IDLE and re-confirm the current drops back down.
7. The difference between the ACTIVE reading and the IDLE reading is the baseline M55 current saved purely by the idle thread's automatic WFI in this SDK. Record this value — it becomes the baseline for comparing current/latency measured in Lab 02 (watchdog) and Lab 04 (PMU_EN wake).

## Items Needing Confirmation (TBD)

1. Whether the board has a dedicated measurement point that isolates just the CPU/SoC current.
2. The exact physical connector designation for the M4 console.
3. Whether the `ov02c10` node actually exists in the M4 devicetree (assumed not to, and excluded from the M4 overlay on that assumption).
4. Whether the `k_yield()`-based ACTIVE loop precisely matches the assumption of "the CPU stays 100% active" on this board's actual clock/power-management hardware needs final confirmation by measurement (in theory, simply preventing the idle thread from being scheduled should be sufficient).

SDK issues found on hardware during implementation (console RX contamination, the `gpio-keys` driver bug, etc.) and their diagnostic process are documented separately in `01_baseline_active_idle_troubleshooting_en.md`.

## Next Lab Preview (Lab 02)

Lab 02 enables M55's dedicated watchdog (`wdog0`) in the board overlay and empirically verifies whether a real SoC-level reset actually occurs when `wdt_feed()` stops being called, and whether the time to that reset matches the theoretical value (2x the configured timeout). This is the first case of a "deep sleep that restarts completely from scratch (reset)," in contrast to the "shallow sleep that resumes execution with no reset (WFI)" demonstrated by this lab (Lab 01).

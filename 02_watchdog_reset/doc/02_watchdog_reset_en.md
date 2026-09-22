# Lab 02 — M55's Own Watchdog: Does Stopping the Feed Really Cause a Reset?

## Learning Objectives

- Work hands-on with M55's dedicated hardware watchdog (`wdog0`) through the standard Zephyr Watchdog API (`wdt_install_timeout()`, `wdt_setup()`, `wdt_feed()`).
- Directly confirm on hardware the difference between `WDT_FLAG_RESET_NONE` (callback only, no real reset) and `WDT_FLAG_RESET_SOC` (a real SoC reset).
- Deliberately stop calling `wdt_feed()` and measure whether a reset actually happens, and whether its timing matches theory (twice the configured timeout).
- Confirm that `hwinfo_get_reset_cause()` reports `RESET_WATCHDOG` as the reboot cause, and practice the standard pattern of always checking this API at the very start of every boot.
- Watch a global variable snap back to its initial value after the reset, to feel — in contrast with Lab 01's WFI (context preserved) — that this reset is a **full cold reboot starting over from scratch**, not a "warm resume" from where it left off.

## Connection to Lab 00/01

Lab 00 (the SDK source review) confirmed that `wdog0` (`compatible = "syna,watchdog"`, `dts/arm/syna/astra_sr/sr100_m55.dtsi`) implements the standard Zephyr `wdt_driver_api` (`setup`/`disable`/`install_timeout`/`feed`). Two confirmed facts from that review matter especially here.

1. **`WDT_FLAG_RESET_CPU_CORE` is explicitly rejected by this driver (`-EINVAL`)** — "reset only this core" is not supported. Only `WDT_FLAG_RESET_NONE` (callback only, no real reset) or `WDT_FLAG_RESET_SOC` (a real SoC reset) are supported. **Hardware-confirmed (2026-09-21)**: this constraint turned out to be literal — `RESET_SOC` is a reset that halts **the whole SoC, M4 included, not just M55** (see "Hardware Setup" below and troubleshooting doc issue 2). This lines up exactly with why `CPU_CORE` was never supported in the first place — resetting M55 alone appears to be simply impossible on this hardware.
2. At the time of Lab 00's source review, we assumed that "once the counter expires, the callback fires, and if not fed, the second expiry (2x the configured timeout) causes either a real reset or, under `RESET_NONE`, an infinite loop." **Testing `RESET_NONE` on hardware in this lab showed that assumption was wrong** — there is no infinite loop and no reset; instead the callback simply keeps firing again every timeout period, indefinitely (see "Code Structure" below and troubleshooting doc issue 1). Whether `RESET_SOC` actually causes a reset needed a separate check, covered as Step 2 in this document's "Running the Lab and Checking Results" section.

Lab 01 covered "shallow sleep (WFI) that resumes without a reset." Lab 02 is the opposite case — the first example of **"deep sleep (reset) that starts completely over from scratch."** Even though both end in "the SoC is running again," WFI preserves registers/RAM and resumes exactly where it left off, while a watchdog reset is a genuine cold boot (`main()` restarts, globals reinitialize) — that contrast is the core point of this lab.

## Hardware Setup

- The M55 console alone is enough (230400bps 8N1, or 115200bps over the onboard bridge — same as Lab 01).
- Keeping the M4 console open too is recommended. ~~The original idea was to confirm M4's heartbeat keeps ticking uninterrupted while M55 resets itself via its own watchdog, proving the reset is local to M55~~ — **hardware testing (2026-09-21) disproved this.** M4's heartbeat also halts when `WDT_FLAG_RESET_SOC` fires. In other words, this watchdog's "SoC reset" is exactly what it says — a reset that halts **the whole SoC, M4 included**, not just M55. The reason for watching the M4 console has flipped: it's no longer "confirm M4 keeps running," it's now **"confirm M4 halts too."** See troubleshooting doc issue 2 for details.
- No extra wiring (buttons, sensors) is needed.

## Devicetree Overlay

`wdog0` starts out as `status = "disabled"` in the base devicetree, so this is the first lab in this curriculum to turn it on via an overlay.

`lab/boards/sr100_rdk_sr100_m55.overlay`:
```dts
&wdog0 {
	status = "okay";
};
```

This lab uses none of the shared M4/M55 resources (I2C1, etc.), so the bus-sharing issues covered in Lab 01 don't apply here. No M4-side overlay is needed.

## Code Structure

### M55 (`lab/src/main.c`)

A single compile-time switch at the top of the file toggles between the two variants:

```c
#define WDT_TEST_USE_RESET_SOC 0   /* 0 = WDT_FLAG_RESET_NONE, 1 = WDT_FLAG_RESET_SOC */
```

**Common flow**:
1. Right at `main()`'s entry, `hwinfo_get_reset_cause()` is called to print this boot's cause as a decoded bitmask (`RESET_PIN`/`RESET_WATCHDOG`/`RESET_SOFTWARE`/`RESET_POR`/`RESET_LOW_POWER_WAKE`), then `hwinfo_clear_reset_cause()` resets it. This is the standard boot-time pattern later labs (Lab 05) will reuse.
2. A `static uint32_t boot_local_counter = 12345;` is declared and printed as-is. This variable preserves nothing across resets in any way (no battery-backed RAM was added for this lab) — **the fact that it always prints back as 12345 on every single boot is itself the proof that this reset is a full cold reboot.**
3. `wdt_install_timeout()` registers a timeout (`WDT_TIMEOUT_MS = 2000`) and a callback, with `flags` set to either `WDT_FLAG_RESET_NONE` or `WDT_FLAG_RESET_SOC` depending on the switch above. `wdt_setup()` starts the watchdog.
4. `wdt_feed()` is called 5 times at 1-second intervals first, to demonstrate that feeding really does prevent the timeout from firing.
5. Then feeding is **deliberately stopped**; the `k_uptime_get_32()` value at that moment is printed, followed by an infinite "still alive" heartbeat every 250ms.
6. The callback (`wdt_callback`) fires once at the first expiry and logs it (its call count is also printed via `callback_count`). **Hardware-confirmed result (2026-09-21)**: in the `RESET_NONE` build, there is no reset and no hang afterward — this callback keeps repeating every `WDT_TIMEOUT_MS`, and `main()`'s heartbeat log keeps going indefinitely as well — the original design assumption ("stops in an infinite loop") turned out to be wrong (see troubleshooting doc issue 1). Whether the `RESET_SOC` build actually causes a reset had not yet been tested on hardware at this point.

### M4 (`lab/remote/src/main.c`)

The same 1-second heartbeat as Lab 01 — unchanged. In this lab, M4's role is to serve as a control group showing whether or not it keeps running while M55 resets.

## prj.conf

`lab/prj.conf` (M55) — two additions for the watchdog and reset-cause APIs:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_WATCHDOG=y
CONFIG_HWINFO=y
```

`lab/remote/prj.conf` (M4) — identical to Lab 01, unchanged.

## Build Instructions

Same procedure as Lab 01 (build M4 first, then package it into M55).

```bash
# 1) Build M4 first.
west build -p always -b sr100_rdk/sr100/m4 -d m4 02_watchdog_reset/lab/remote

# 2) Build M55, packaging the M4 image together with it.
west build -p always -b sr100_rdk/sr100/m55 -d m55 02_watchdog_reset/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

After changing `WDT_TEST_USE_RESET_SOC`, only the source changed, so a plain rebuild is usually enough — but if you see odd behavior, confirm you're using `-p always` (Lab 01 already ran into build-cache issues after overlay changes).

## Running the Lab and Checking Results

### Step 1 — `WDT_FLAG_RESET_NONE` (default, `WDT_TEST_USE_RESET_SOC 0`)

**Hardware-confirmed result (2026-09-21)**: the procedure below was originally written expecting (based on Lab 00's source review) "the callback fires once, then the system stops in an infinite loop" — but hardware testing showed **a completely different result**. The callback does not fire once and stop; instead **it keeps firing again every `WDT_TIMEOUT_MS` (2000ms), and `main()`'s heartbeat log never stops either, continuing indefinitely — there is no reset and no hang.** See the troubleshooting doc (issue 1) for the detailed cause and analysis. The procedure below has been updated to reflect this corrected result.

1. Build/flash as above and watch the M55 console.
   ```
   === Lab 02: M55 Watchdog (wdog0) Reset Test ===
   Build variant: WDT_TEST_USE_RESET_SOC=0 (WDT_FLAG_RESET_NONE)
   [boot] reset cause bitmask = 0x00000000 -> (none reported)
   [boot] boot_local_counter = 12345 (...)
   [main] watchdog armed: timeout=2000 ms, channel=0
   [main] fed watchdog (1/5), uptime=1004 ms
   [main] fed watchdog (2/5), uptime=2008 ms
   ...
   [main] fed watchdog (5/5), uptime=5020 ms
   [main] STOPPING FEED NOW at uptime=5020 ms -- watching for timeout/reset
   [main] still running, 250 ms since last feed (callback_fired=0, callback_count=0)
   ...
   >> [WDT] callback fired on channel 0 (call #1) -- NOT feeding, watching what happens next
   >> [WDT] RESET_NONE mode: no SoC reset will occur -- expect this callback to keep repeating every 2000 ms indefinitely (see troubleshooting doc, issue 1)
   [main] still running, ... ms since last feed (callback_fired=1, callback_count=1)
   ... (about 2000ms later)
   >> [WDT] callback fired on channel 0 (call #2) -- NOT feeding, watching what happens next
   ... (this pattern keeps repeating -- it never stops)
   ```
2. Confirm that the callback log (`>> [WDT] callback fired ...`) first appears roughly 2000ms (the configured `WDT_TIMEOUT_MS`) after the last feed.
3. **Confirm that the callback keeps repeating roughly every 2000ms after that, with `[main] still running, ...` printing alongside it and `callback_count` climbing 1, 2, 3, ...** No reboot banner appears (since there's no reset) — but the system doesn't hang either (since there's no hang). This is the hardware-confirmed result, differing from the original design assumption (stopping in an infinite loop).
4. (Optional) If you were also watching the M4 console, `[M4 heartbeat]` keeps climbing independently of M55 — though since M55 itself never halts here, this check doesn't carry much weight in this step.

### Step 2 — `WDT_FLAG_RESET_SOC` (change `WDT_TEST_USE_RESET_SOC` to 1, rebuild/reflash)

**Hardware-confirmed result (2026-09-21) — an important, unexpected outcome**: the procedure below was originally written around the hypothesis that "a reboot banner reappears roughly at 2x the timeout after the callback" — hardware testing showed something different.

- The 5 feeds → stop → callback log (`>> [WDT] callback fired ...`) all appeared as expected.
- But after the callback, **`main()`'s `[main] still running, ...` heartbeat log stopped completely** — no further log of any kind appeared on the console, and no reboot banner (`*** Booting Zephyr OS ***`) ever reappeared.
- At the same time, **the M4 console was also frozen** (`[M4 heartbeat]` stopped climbing too).
- From this halted state, **pressing the board's physical reset button caused a normal reboot.**

In other words, `WDT_FLAG_RESET_SOC` is exactly what it says — **a reset that halts the entire SoC, M4 included, not just M55** — and, as far as we could confirm, **that reset never completes on its own (no reboot banner ever reappears); the system simply stays halted.** Only an external physical reset (a pin reset) brings it back. In other words, this watchdog does not behave as "a safety net that automatically recovers itself on timeout" — if anything, triggering it appears to create a halted state that never recovers without external intervention. See troubleshooting doc issue 2 for the detailed analysis and remaining open questions.

**The steps below are kept for reference, but note that step 4 (checking the reset cause) was never reached the way originally planned** — steps 3–6 assumed "if it reboots automatically," and in practice, only pressing the physical reset button gets you there.

1. The same 5 feeds → stop → callback log appear identically to Step 1.
2. Check whether `[main] still running, ...` stops completely after the callback log, and (if you're also watching the M4 console) whether `[M4 heartbeat]` stops too.
   ```
   [main] STOPPING FEED NOW at uptime=5066 ms -- watching for timeout/reset
   ...
   >> [WDT] callback fired on channel 0 (call #1) -- NOT feeding, watching what happens next
   >> [WDT] RESET_SOC mode: a real SoC reset MAY follow around 2000 ms from here -- not yet confirmed on this SDK
   (both M55 and M4 consoles go completely silent here -- no further logs)
   ```
3. Wait a while (e.g. 30 seconds to a minute) and confirm the reboot banner never reappears (i.e. there's no automatic recovery).
4. Press the board's **physical reset button** and confirm it reboots normally.

**Hardware-confirmed result (2026-09-21)**: after rebooting, the `reset cause bitmask` was `0x00000008 -> RESET_POR` — neither `RESET_WATCHDOG` nor `RESET_PIN`. To check whether this was actually related to the watchdog, we pressed the same button in a normal state unrelated to this lab, and got **the exact same `RESET_POR`** — meaning **this board's physical System Reset button reports `RESET_POR` regardless of whether a watchdog was involved.** As a result, using `hwinfo_get_reset_cause()` to determine "whether the watchdog's SoC reset request was actually acted on by the hardware" turns out to be impossible with this button — that's the final conclusion here. See troubleshooting doc issue 2 for details.
5. `boot_local_counter` was confirmed to still print `12345` after the physical reset (the cold reboot itself is normal), but whether this reboot was "caused by the watchdog" or "just the button's own POR" cannot be distinguished, for the same reason as step 4 above.

## Open Items (TBD)

1. ~~Whether the `reset cause bitmask` after recovering via physical reset is `RESET_WATCHDOG` or `RESET_PIN`~~ — **Confirmed (2026-09-21): neither — it's `RESET_POR` (`0x00000008`), and a baseline test confirmed this is a property of this board's physical reset button itself (always POR, watchdog or not), so this diagnostic path is a dead end** (see troubleshooting doc issue 2).
2. ~~Whether `RESET_SOC` mode actually causes a reset~~ — **Confirmed (2026-09-21), final conclusion**: both M55 and M4 halt after the callback, confirmed — but **that reset was never observed to complete and reboot on its own; only a physical reset button press recovers it.** Whether that reset request actually executed at the hardware level cannot be determined via reset cause, because this board's physical reset button always reports `RESET_POR` — see troubleshooting doc issue 2.
3. Whether the behaviors confirmed here — "auto re-arm on every timeout" (`RESET_NONE`) and "a reset is triggered but never auto-recovers" (`RESET_SOC`) — hold the same way at shorter (e.g. 100ms) or longer `WDT_TIMEOUT_MS` values, and what the driver/SDK's supported minimum/maximum timeout range is (e.g. `WDT_SYNA_DEFAULT_TIMEOUT_MAX`), were not checked.
4. ~~Whether the driver "spins forever" in `RESET_NONE` mode~~ — **Confirmed (2026-09-21), differs from the prior assumption**: it does not spin forever; instead the callback keeps re-firing every timeout period while the system keeps running normally. The exact internal mechanism (whether the driver always re-arms the counter after running the callback, or the counter auto-rearms as part of clearing the interrupt, etc.) could not be pinned down without re-reading the source — see troubleshooting doc issue 1.
5. ~~Whether `RESET_SOC` is a reset local to M55 alone~~ — **Confirmed (2026-09-21), differs from the prior assumption**: it's a whole-SoC reset that halts M4 as well — see troubleshooting doc issue 2. This directly contradicts what the Lab 00/01 docs had been saying — that "M4 is Always-On and unaffected by M55's power state" — so it's an important correction that may also affect other labs, especially future IPC/Low-Power-Wake labs.

## Coming Up Next (Lab 03)

Lab 03 reuses the mbox pattern from the existing M4↔M55 IPC curriculum, where M4 only notifies M55 when a sensor threshold is exceeded. Following Lab 01 (WFI, no reset) and Lab 02 (watchdog, with reset), this introduces a third kind of wake — "no reset, not WFI either, but woken by another core while staying alive." In this lab, M4's workload is also replaced for the first time, moving from Lab 01/02's fixed heartbeat to actual sensor polling.

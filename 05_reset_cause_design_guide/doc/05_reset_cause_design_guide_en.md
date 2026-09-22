# Lab 05 — Synthesis: A Reset-Cause-Based Design Guide

**Status: hardware-confirmed, finalized (2026-09-22).**

This is the final lab in the curriculum. Rather than exploring a new hardware phenomenon, it synthesizes everything confirmed in Labs 00-04 into a single piece of representative code and a design guide — "this is how a real product would write it."

## Learning goals

- Practice the **standard boot handling pattern** of always checking `hwinfo_get_reset_cause()` right at boot and branching on why this particular boot happened.
- Synthesize Labs 01-04 into a single comparison table covering each confirmed "wake" mechanism's trigger, resume behavior (context preservation), relative responsiveness, and hardware-verification status.
- Explicitly state the conclusion that this SDK has **no intermediate deep-sleep state that preserves context** (a Zephyr `PM_STATE_SUSPEND_TO_RAM`-type state) — there are only two levels: WFI (shallow, resumes immediately) or reset (deep, full reboot).
- Close out the curriculum with representative code that reproduces this SoC's original design intent: **M4 watches Always-On, and only wakes M55 when a meaningful event occurs.**

## Connection to previous labs

This lab ties off, in code, the final answers to the three questions Lab 00 originally posed (section 1-3).

- Lab 01 accepted, on hardware, that this SDK has no `CONFIG_PM` (Lab 00), and established a baseline using nothing but the pure idle thread's automatic WFI — **"shallow wake."**
- Lab 02 confirmed M55's dedicated watchdog on hardware, but closed with an unexpected result: `WDT_FLAG_RESET_SOC` was not confirmed to be a self-completing safety reset — **"deep wake (reset)," but incomplete.**
- Lab 03 reused an mbox pattern from a separate IPC curriculum and confirmed, exactly as intended, that M4 can wake M55 **with no reset, while it stays alive** — this is effectively the answer to the "official path for M4 to wake M55" that this curriculum was originally looking for.
- Lab 04 explored the `SW7`/`PMU_EN` path, but confirmed that verifying a "meaningful wake-up" is out of scope, since this SDK has no software entry point into a low-power mode at all.

This lab combines Lab 01's baseline concept, Lab 03's mbox pattern (which reliably works), and Lab 02's boot-cause diagnostic pattern into one, while **stating — not avoiding — the two limitations confirmed in Lab 02/04** (a reset that doesn't self-complete / no low-power-mode entry path) directly in the code comments.

## Hardware setup

Same as Lab 03 — M4 masters I2C1 to read the onboard accelerometer (`mc3479`), and M55 only opens the mbox rx side. Keep both the M55 and M4 consoles open.

## Devicetree overlay

Lab 03's overlays are **reused as-is** (M55: `i2c1`/`gpio_exp0`/`ov02c10` disabled; M4: `mc3479` enabled; `ipc0` shared-memory-size 0x400 matching on both sides).

## Code structure

### M55 (`lab/src/main.c`)

This has two parts.

1. **`handle_boot_reset_cause()`** — the same decoding pattern used in Lab 02/04, with a one-line comment added per category on "what a real product should actually do about this."
   - `RESET_PIN`/`RESET_SOFTWARE`: a normal cold start.
   - `RESET_WATCHDOG`: since Lab 02 confirmed this SDK/board does not self-complete this reset, a comment noting that if this cause is seen repeatedly in practice, it should be treated as a **real fault signal worth logging/counting**, not routine housekeeping.
   - `RESET_POR`: a comment noting that on this board, both the physical System Reset button (Lab 02) and a (unconfirmed) PMU_EN-driven wake (Lab 04) can both be reported under this same bit, so this bit alone cannot distinguish between them.
   - `RESET_LOW_POWER_WAKE`: a comment noting that this curriculum never managed to reproduce this from software (Lab 04), so if this value is ever seen for real, it came from actual RTC/PIR hardware, not from this code.
2. **The mbox event loop** — Lab 03's M55 code, reused **exactly as-is**. After calling `handle_boot_reset_cause()`, it opens the mbox rx channel and waits on `k_msgq_get(..., K_FOREVER)` — logging whenever an M4 event arrives, and sitting in WFI in between.

**Deliberately left out (with the reason stated in the code's header comment)**:
- Lab 02's `WDT_FLAG_RESET_SOC` is not armed here — now that it's confirmed to cause a stopped state that only physical intervention can recover from, including it in a "design guide" capstone would demonstrate a foot-gun, not a recommended pattern.
- Lab 04's `SW7`/`PMU_EN` path is not reproduced either — this SDK version simply has no API to enter the HW low-power mode that path is meant to wake from.

### M4 (`lab/remote/src/main.c`)

Lab 03's code is reused **completely unchanged** — only the header comment gained context noting "this is the confirmed, working Always-On design this curriculum found"; not a single line of logic changed.

## prj.conf

`lab/prj.conf` (M55) — combines Lab 02's `CONFIG_HWINFO=y` with Lab 03's `CONFIG_MBOX=y`:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_HWINFO=y
CONFIG_MBOX=y
```

`lab/remote/prj.conf` (M4) — identical to Lab 03, unchanged.

## How to build

Same approach as previous labs.

```bash
# 1) Build M4 first.
west build -p always -b sr100_rdk/sr100/m4 -d m4 05_reset_cause_design_guide/lab/remote

# 2) Build M55, packaging the M4 image together with it.
west build -p always -b sr100_rdk/sr100/m55 -d m55 05_reset_cause_design_guide/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

## Running the lab and checking results

**Hardware-confirmed (2026-09-22)**: both `handle_boot_reset_cause()`'s boot-cause classification output at boot, and the STILL/ACTIVE mbox event logs when shaking the board (the same pattern as Lab 03), worked exactly as expected. With this final lab working correctly, it's also confirmed that everything established in Labs 00-04 (boot-cause decoding, mbox-based Always-On wake) merges into a single codebase with no issues.

## Lab 01-04 comparison table

| Lab | Wake/resume type | Trigger | Context preserved | Relative responsiveness | Hardware-verification status |
|---|---|---|---|---|---|
| Lab 01 | WFI (idle thread, automatic) | `k_msleep()` timer expiry | **Fully preserved** — resumes immediately where it left off | Effectively instant (interrupt-latency level) | Hardware-confirmed |
| Lab 02 | Watchdog reset (`RESET_SOC`) | Missed feed (timeout) | **None** — cold reboot from `main()` | **Not confirmed** — does not self-complete; stays stopped indefinitely until a physical reset | Hardware-confirmed, but not confirmed to be a "self-completing safety reset" (incomplete result) |
| Lab 03 | mbox event (no reset) | M4's state-change notify | **Fully preserved** — handled immediately at the wait point, no reset | Effectively instant (mbox interrupt-based) | Hardware-confirmed, worked smoothly |
| Lab 04 | `SW7`/`PMU_EN` (unconfirmed) | Physical button (presumed PMU_EN) | N/A — the trigger itself is never observed while the SoC is powered on | N/A | Confirmed to produce no effect while the SoC is powered on. Anything beyond that (a genuine wake from a low-power mode) is out of scope for this SDK version |

Current (mA) figures are not arbitrarily re-entered in this synthesis document — for the exact measured values, see the measurement log in the Lab 01 doc (`01_baseline_active_idle_en.md`). Qualitatively, Lab 01/03 showed clearly lower current than ACTIVE during their IDLE/WFI intervals; Lab 02's stopped state and Lab 04's unconfirmed state were not separately measured.

## Final design implications for this SDK/board

- **There is no intermediate deep-sleep state that preserves context.** Of the seven Zephyr System Power States (section 1-1), the only one this SDK effectively implements is `PM_STATE_RUNTIME_IDLE` (WFI); the next step up jumps straight to a `PM_STATE_SOFT_OFF`-class full reboot (watchdog reset). There is no middle ground in this SDK version, like `PM_STATE_STANDBY`/`SUSPEND_TO_RAM`, that keeps part of the system on while preserving context.
- **The only officially confirmed, reliably working way for M4 to wake M55 is mbox (Lab 03).** The `RESET_LOW_POWER_WAKE`/`PMU_EN` path (Lab 04) is confirmed to exist at the hardware level, but there is no way to drive it from software.
- **Watchdog-based reset (Lab 02) is hard to trust as a safety mechanism.** Once triggered, it leaves the system in a stopped state with no guaranteed automatic recovery. If a real product needs a safety mechanism, this SDK version alone is not enough, and an external watchdog circuit (an external watchdog IC, etc.) may need to be considered.
- These three points are the current limitations — and the design guide — that this curriculum (Lab 00-05) established on hardware for power/wake management on the SR110 + Zephyr (`syna_zephyr_sdk-1.0.0`) combination.

## Items to verify (TBD)

- (Optional) A separate reproduction test to confirm the `RESET_WATCHDOG` branch actually fires in that situation — not required, and not attempted before closing out the curriculum.
- If you'd like the exact ACTIVE/IDLE current (mA) figures from Lab 01 reflected in this document's comparison table, share the measurement log and the table can be updated.
- The hardware-level Active/Low-power/Ultra-low-power mode transitions this curriculum originally wanted to verify on SR110 could not be included as a hands-on lab, since this SDK version has no such API — see the "Curriculum wrap-up" section below.

## Curriculum wrap-up

The final answers to the three questions Lab 00 originally posed — "Can M4 wake M55 via a GPIO interrupt?", "Is mbox the only path?", "What are M55's own wake triggers?" — are: a GPIO interrupt path was never confirmed at the software level; **mbox is the only M4→M55 wake path this SDK hardware-verified**; and none of M55's own wake triggers (button/RTC/PIR/watchdog) were confirmed to work reliably on hardware (watchdog is incomplete, and the PMU_EN family produces no effect while the SoC is powered on). This curriculum doesn't close on an optimistic note — it closes with **an honest, hardware-verified line drawn between what actually works and what doesn't on this SDK version.**

**What we wished we could have covered (an honest retrospective)**: the original goal when starting this curriculum was to hands-on exercise the Active / Low-power / Ultra-low-power mode transitions that SR110 appears to support at the hardware level. But as Labs 00-04 confirmed, this SDK version (`syna_zephyr_sdk-1.0.0`) has no software API to enter those modes at all (no `CONFIG_PM`). So in the end, what this curriculum could actually cover, at the Zephyr/software level, was only **toggling between Idle → Sleep (WFI) → back to Active** (Lab 01), plus two reset-based paths (watchdog reset — Lab 02, the PMU_EN family — Lab 04). In particular, even the **watchdog-based power reset we hoped could serve as a safety mechanism turned out not to self-complete** (Lab 02), so this curriculum on its own did not reach the original goal of a genuine low-power-mode transition. Actually opening up this hardware-level low-power mode will likely require a future version of the Synaptics SDK, or a direct inquiry to them — this curriculum is left as the hardware-verified record of exactly how far that boundary was reached.

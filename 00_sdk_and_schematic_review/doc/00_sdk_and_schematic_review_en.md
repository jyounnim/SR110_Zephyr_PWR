# Lab 00 — SR110 Power Mode: SDK and Schematic Source Review

## Learning Objectives

- Understand the concept of Zephyr's standard System Power States (`enum pm_state`).
- Determine, by directly reading the `syna_zephyr_sdk-1.0.0` source code, which of these standard power states are actually implemented on the SR110 (SR100 SoC) board.
- Use the SR110 RDK schematic (SC950-C01116-01 RevE) to identify hardware-level power sequencing/wake trigger structures that are invisible at the software (SDK) level.
- Organize the various paths by which M4 (the Always-On core) wakes M55 (the core whose power state actually changes), and classify each path as either "wake with a reset" or "wake that resumes execution with no reset."
- Clearly identify the items (TBD) that must be verified on hardware in later labs.

This lab has no code. It is a research lab that organizes findings from reading and analyzing the SDK source and the board schematic; hands-on code labs begin with Lab 01.

## Why start with a source/schematic review instead of code

Of SR110's two cores (M55, M4), **M4 is Always-On, and only M55's power state actually changes.** This curriculum's core questions are "how does M4 wake M55?" and "once M55 wakes, does it resume execution right where it left off, or does it reboot from scratch?"

To answer these questions, we first need to confirm whether this SDK version actually implements Zephyr's standard Power Management subsystem (`CONFIG_PM`). Whether it does or not completely changes the scope that later labs can cover. We also need to check the schematic for board-level physical wake triggers (buttons, RTC, PIR sensor, etc.) that are invisible from the SDK source alone, so that we don't confuse a software-implemented path with a path that exists only in hardware.

## 1. Scope of Investigation

- **SDK**: `syna_zephyr_sdk-1.0.0` (provided by the user; fully reviewed after extraction)
- **Schematic**: `SC950-C01116-01 RevE` (SR110 RDK board, all 13 pages)

## 2. Zephyr Standard System Power States (General)

Zephyr defines a 7-level System Power State model via `enum pm_state`. The table below reflects the general definitions from Zephyr's official documentation; section 3 confirms which of these are actually implemented in this SDK.

| State | Definition | Behavior after wake (general) |
|---|---|---|
| `PM_STATE_ACTIVE` | Fully powered, running normally | — |
| `PM_STATE_RUNTIME_IDLE` | All cores waiting in the shallowest idle (WFI) | Resumes immediately from where it left off as soon as an interrupt occurs |
| `PM_STATE_SUSPEND_TO_IDLE` | The whole system goes through a normal suspend procedure, putting all cores in idle | Similar to RUNTIME_IDLE, resumes immediately |
| `PM_STATE_STANDBY` | Peripherals enter low power + all cores other than the boot CPU are power-gated | Power-gated cores re-boot |
| `PM_STATE_SUSPEND_TO_RAM` | Memory preserved via self-refresh, everything else power-gated as much as possible | "Warm resume" with RAM contents preserved |
| `PM_STATE_SUSPEND_TO_DISK` | Most power removed, including memory | Reloaded from storage media — effectively a reboot |
| `PM_STATE_SOFT_OFF` | Minimum power state, large latency to return | Effectively identical to a cold boot restarting from `main()` |

The notable point in this table is that between `RUNTIME_IDLE`/`SUSPEND_TO_IDLE` (resumes execution with no reset) and `SOFT_OFF` (effectively a reboot), there exist intermediate deep-sleep states (`SUSPEND_TO_RAM`, `STANDBY`) that still preserve RAM contents. Section 3 confirms whether this SDK supports these intermediate states.

## 3. `syna_zephyr_sdk-1.0.0` Source Review Results (Confirmed Facts)

Every item below is a fact confirmed by directly opening the SDK source code, not a guess. The source file path is given as evidence for each item.

### 3.1 `CONFIG_PM` itself is not implemented in this SDK

An exhaustive grep across the entire SDK tree for the strings `pm_state_set`, `PM_STATE_*`, `power-states`, and `zephyr,power-state` found no matches anywhere. There are also no PM-subsystem-related samples or devicetree `power-states` nodes.

**Conclusion**: none of the 7 System Power States from section 2 are implemented in this SDK version. The only low-power mechanism available to an application is the automatic WFI (Wait For Interrupt) entry that Zephyr's idle thread provides by default, which always operates regardless of the `CONFIG_PM` setting.

### 3.2 A dedicated M55 watchdog exists and is exposed via the standard Zephyr WDT API

- Node: `wdog0: watchdog@50350038`, `compatible = "syna,watchdog"` — `dts/arm/syna/astra_sr/sr100/sr100_m55.dtsi`. It defaults to `status = "disabled"` and must be explicitly enabled in a board overlay.
- M4 has a separate watchdog (`lp_wdt: watchdog@b5003000`, `compatible = "arm,apb-wdt"` — `sr100_m4.dtsi`). M4 and M55 use different watchdog IP blocks, but both write their reset-cause bit into the same shared register, `AON_POR_RST` (address `0x50350038`, `soc/syna/astra_sr/sr100/soc.c`): M4 watchdog = bit 8 (`AON_POR_RST_M4_WATCHDOG_BIT`), M55 watchdog = bit 9 (`AON_POR_RST_M55_WATCHDOG_BIT`).
- The driver (`drivers/watchdog/wdt_syna.c`) implements the standard `wdt_driver_api` (`setup`/`disable`/`install_timeout`/`feed`).
- **`WDT_FLAG_RESET_CPU_CORE` is explicitly rejected (returns `-EINVAL`)** — "reset only this one core" is not supported. Only two options are supported: `WDT_FLAG_RESET_NONE` (only invokes the callback; if not fed afterward it simply enters an infinite loop, with no actual reset) and `WDT_FLAG_RESET_SOC` (resets the whole SoC).
- Per the driver's comments, once the counter first expires, the registered callback (if any) is invoked; if the watchdog is not fed inside that callback, an actual reset occurs at the **second expiration point (after twice the configured timeout has elapsed)**.

**Conclusion**: an M55 watchdog timeout is a genuine SoC-level hardware reset. It is not "resume execution where it left off" — it is a **full cold reboot that restarts from `main()`**. This is exactly the "goes into ultra-low power, then wakes up by rebooting" scenario the user originally hypothesized.

### 3.3 `hwinfo_get_reset_cause()` distinguishes 5 reset causes via the standard Zephyr API

Evidence: `drivers/hwinfo/hwinfo_syna_sr100.c`.

| Register bit mask | Standard Zephyr flag | Meaning |
|---|---|---|
| `SR100_RST_PIN_MASK` (bit 12) | `RESET_PIN` | Physical reset pin/button |
| `SR100_RST_POR_MASK` (bit 13) | `RESET_POR` | Power-on reset from the analog PMU |
| `SR100_RST_WDOG_MASK` (bits 8-9) | `RESET_WATCHDOG` | M4 or M55 watchdog (section 3.2) |
| `SR100_RST_SW_MASK` (bits 10-11) | `RESET_SOFTWARE` | Reset triggered by software |
| `SR100_RST_AON_LP_MASK` (bits 0-7) | `RESET_LOW_POWER_WAKE` | Low-power wake triggered by the AON (Always-On) or M4/LPPROC domain |

`hwinfo_get_reset_cause()`/`hwinfo_clear_reset_cause()` (writes `0x3fff` on clear) implement this API.

**The mere existence of `RESET_LOW_POWER_WAKE` is the key piece of evidence.** This SoC has a concept of "a low-power wake triggered by the M4/AON domain, distinguished at the hardware level from watchdog/pin/software resets." In other words, **an official, hardware-level path for M4 to wake M55 does actually exist**, and — as the register's own name, "reset cause," suggests, just like the other reset causes — **this wake, too, is accompanied by a reset.**

However, this SDK version has no API or Kconfig option that lets application code actively trigger this cause. Discovering on hardware exactly what triggers it is Lab 04's goal.

### 3.4 The AON Event GPIO is not for M4↔M55 communication — it's a debug-only output for observing the internal PMU state machine

Evidence: `drivers/misc/syna_aon_event_gpio/syna_aon_event_gpio.c`, `include/zephyr/dt-bindings/misc/syna_aon_event_gpio.h`. The node `aon_event_gpio: gpio@50350060` (`compatible = "syna,aon-event-gpio"`) exists only in the M55 dtsi.

The event-definition header defines things like `AON_GPO2_EVENT_LPPROC_SM_WAKE` (M4 = LPPROC domain) and `AON_GPO2_EVENT_MAIN_SM_WAKE` (M55 = MAIN domain), confirming that **a real hardware state machine that manages each of M4's and M55's power domains exists inside this SoC.**

The driver is initialization-only code that configures the register (event/pulse-width/polarity) once at boot, and there is no runtime API for an application to read or write. In other words, **it cannot be used in software to exchange M4↔M55 signals.** However, it is useful as a verification aid (used as a supporting tool in Lab 04) — connecting an oscilloscope or logic analyzer to this pin lets you visually observe exactly when MAIN_SM (M55) / LPPROC_SM (M4) actually switch between WAKE/IDLE states.

### 3.5 The board has a physical, dedicated "Wake-up" button, but there is no corresponding devicetree node

Evidence: `boards/syna/astra_sr/sr100/doc/index.rst` contains the phrase "Push buttons for system reset and wake-up." However, there is no GPIO node anywhere in the devicetree corresponding to this button.

This suggests it may be a **hardware-level button that operates before Zephyr runs, at the PMU/power-sequencing stage**, rather than a GPIO that an already-booted application polls or receives interrupts from. This possibility is supported by evidence found in the schematic review in section 4.

### 3.6 There is no software API anywhere in the SDK for power/reset control in the M4→M55 direction

`soc_late_init_hook()` in `soc/syna/astra_sr/sr100/soc.c` contains only `CONFIG_SR100_RELEASE_M4_RESET` (an option for M55 to release M4 from reset after boot, by clearing the `(0x3 << 8)` bits of the `AON_CONFIG` register); there is no corresponding "M55 release/reset" option anywhere in the SDK (confirmed by an exhaustive check of `Kconfig.soc`).

**Conclusion**: there is no example of "M4 wakes M55" actually implemented in code, neither in this SDK nor in the separate M4↔M55 IPC curriculum previously done — this lab (Lab 00) is the first to address this area.

### 3.7 M4/M55 parallel build method

Evidence: `samples/m4/README.rst`. M4 is built separately as `sr100_rdk/sr100/m4`, and then M55 is built with `-DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD=<M4 build path>` to merge both into a single flash image. This is identical to the method used in the separate M4↔M55 IPC curriculum, and the fact that `M4_BUILD` is resolved as a path relative to the M55 build directory (e.g., `../m4`, depending on workspace layout) was already confirmed there as well.

## 4. SR110 RDK Schematic (SC950-C01116-01 RevE) Review Results

The SDK review in section 3 only tells us facts at the software level, "after Zephyr has booted." But as with the physical wake button mentioned in section 3.5, **hardware paths that only operate before Zephyr runs (during PMU power sequencing)** can only be confirmed by looking at the schematic. This section covers the results of reviewing the 13-page SR110 RDK schematic.

### 4.1 Investigation Method and Limitations

Net names and component information were identified by extracting text from the schematic PDF. This method is valid for identifying signal names and component lists, but **it does not confirm exact on-page coordinates or wire routing.** In other words, the content below establishes only that "these signals and components exist in the schematic, and appear under the same net name across multiple pages" — final confirmation of whether "pin A and pin B are actually connected by a drawn line" requires additional verification, either by reviewing the PDF's drawing pixel-by-pixel or by measuring continuity with a multimeter on the actual board. Wherever this section's content is limited in this way, it is re-flagged in the TBD list in section 7.

### 4.2 `AON_GPI1` / `GPIO3` — an SoC-side AON input pin

- **Page 4** (the SR110 main SoC page): the pin list for `U2 SR110_FCCSP122` (the SoC package) includes a pin named `SR110_GPIO3.AON_GPI1`, and this signal has an off-page cross-reference to **Page 9** (the GPIO Expander page).
- This is a separate fact from what was confirmed in section 3.4 — that "AON state-machine-related signals are exposed on the M55 devicetree (`aon_event_gpio`) primarily as outputs (GPO)" — and is the first schematic-level confirmation that **the SoC also has at least one input (GPI) pin, `AON_GPI1` (shared with GPIO3), coming into the AON domain.** This pin is not exposed as a node in any devicetree file in the SDK (it does not appear in the node list confirmed in section 3).
- **TBD**: whether this pin is actually connected to something observable from M55 (or M4), whether it functions as a general-purpose GPIO the application can read as GPIO3, or whether it is AON-hardware-logic-only and inaccessible from software, could not be determined from this text-based review alone.

### 4.3 Wake-up Trigger Circuit (Page 13) — Button + RTC + PIR → `PMU_EN`

**Page 13** carries the section title "RTC, Wake-up" on the schematic itself, and contains the explicit phrase "Wake up triggers." The components and signals confirmed on this page are as follows.

| Component/Signal | Description |
|---|---|
| `U26 BU9873NUX-TTR` | An I2C-interface RTC (Real-Time Clock) IC. I2C address `0x32`. |
| `RTC_SCL` / `RTC_SDA` | RTC's I2C bus signals |
| `RTC_INTn` | RTC's interrupt output (presumed active-low, based on the `n` suffix in the signal name) |
| `RTC_INT_HI-ACT` | A separate net that appears to be the RTC interrupt converted to active-high |
| `PIR_INT` | Interrupt signal from a PIR (passive infrared motion) sensor |
| `SW7` (Tact_SPST_NO) | A momentary tactile push button. The same component also appears on Page 2 (the power tree page), so it is likely that this `SW7` is the "physical Wake-up button" that section 3.5 only confirmed from SDK documentation. |
| `EXT_INT` | Confirmed to be the point where the above signals combine (appearing to be wired in parallel based on the description text), and is shown as ultimately driving the `SR110_PMU_EN` signal. |
| `SR110_PMU_EN` | A signal presumed to be the SoC's PMU (Power Management Unit) Enable input |

**Interpretation**: this page appears to show the structure "pressing the button (SW7), an RTC alarm firing (`RTC_INTn`), or a PIR sensor detecting motion (`PIR_INT`) all converge at a single point (`EXT_INT`), which drives the SoC's `PMU_EN`." Given that the name `PMU_EN` itself means "turn on the PMU," it is most consistent with the SDK review (section 3.5) to interpret this as **a path that (re-)powers the system before Zephyr runs, at a point when the SoC is not yet fully powered or the PMU is in a low-power state.**

### 4.4 `AON_GPO1` in the Power Tree (Page 2)

**Page 2** (the power tree page) contains the phrase "`JP1` `AON_GPO1` BJT R System Power selection npn," suggesting that the SoC's `AON_GPO1` output passes through a BJT (NPN transistor) and is involved in system power selection/switching. The same page also shows the main power slide switch `SW1` and the buck-boost converter `TPS63900DSKR`.

This signal (`AON_GPO1`) matches the name of one of the GPO pins (`AON_GPO0/1/2`) initialized by the `aon_event_gpio` driver confirmed in section 3.4. In other words, **there is a possibility that this pin, understood from the SDK alone as a "debug observation output," actually also serves a functional role in the schematic, driving a real power-switching transistor.** **TBD**: whether the `aon_event_gpio` driver's event configuration (e.g., `AON_GPO2_EVENT_MAIN_SM_WAKE`) is actually linked to this power-switching behavior, or whether this BJT path is a completely separate, static power-sequencing circuit, could not be determined from this review.

## 5. Summary — Organizing the Paths by Which M4 Wakes M55

An earlier conversation asked three questions to confirm. The answers previously given from the SDK review alone are corrected/reinforced below, incorporating the results of this schematic review.

| # | Question | Answer before schematic review | Answer after incorporating schematic review |
|---|---|---|---|
| 1 | Is there no way to wake M4→M55 via a GPIO interrupt? | Appeared not to exist (no such path in the devicetree) | **Corrected**: `AON_GPI1` (section 4.2), an SoC input pin not exposed in the devicetree, exists in the schematic. It cannot be said to definitively "not exist" — only that "there is no software-accessible path at the SDK/devicetree level" is confirmed. |
| 2 | Is IPC (mbox) the only way to wake M4→M55? | The only path verified in software | **Corrected**: this should be limited to mean "the only software-level path that has been hardware-verified and is actually in use in the M4↔M55 IPC curriculum so far." At the hardware level, at least one more physical wake path exists that is unrelated to mbox, like the `PMU_EN` path in section 4.3. |
| 3 | Is a wake-up button and a watchdog the only way M55 wakes up on its own? | Presumed to be these two: button + watchdog | **Corrected and expanded**: at least 4 triggers are confirmed — ① button (`SW7`), ② RTC alarm (`RTC_INTn`, `U26 BU9873NUX`), ③ PIR sensor (`PIR_INT`), ④ M55 watchdog (section 3.2). Of these, ①-③ appear, per the schematic, to converge at the same point (`EXT_INT`) and share one `PMU_EN` path, while ④ is an entirely different mechanism (see section 6). |

## 6. Two Different Kinds of "Reset" Must Be Distinguished

The most important conclusion of this investigation is that on SR110, the phenomenon of "M55 starting over" can be caused by **at least two mechanisms that are different in nature.**

1. **`PMU_EN`-based cold power-on** (section 4.3): a path where the button/RTC/PIR turns on the SoC's PMU itself via `EXT_INT`. This is closer to **a physical power sequencing event where the SoC is powered up entirely anew (or a low-power PMU is reactivated)**, unrelated to Zephyr or the watchdog register. Whether `hwinfo_get_reset_cause()` classifies this as `RESET_POR` or `RESET_LOW_POWER_WAKE` has not yet been confirmed on hardware.
2. **Watchdog-based SoC reset** (section 3.2): a path where, with the SoC already powered on, an M55 (or M4) watchdog timeout triggers an SoC reset via the `AON_POR_RST` register. `hwinfo_get_reset_cause()` clearly classifies this as `RESET_WATCHDOG` (sections 3.2/3.3, confirmed from SDK source).

Both paths result in "M55 restarting from `main()`," but their causes are entirely different — one is a **PMU/power-level event**, the other is **a software-triggered reset inside an already-powered-on SoC**. Making this distinction clear is why Lab 02 (watchdog) and Lab 04 (exploring the PMU_EN family of wakes) were designed as separate labs.

## 7. Items Requiring Verification (TBD, needs hardware confirmation)

1. What actually triggers `RESET_LOW_POWER_WAKE` — an undocumented bit in the `AON_CONFIG` register, PMU sequencing, or its relationship to the `PMU_EN` path in section 4.3 (explored in Lab 04)
2. Since the `SW7` button is not exposed as any GPIO in the devicetree, what value `hwinfo_get_reset_cause()` reports from a Zephyr application's perspective when this button is pressed (expected to be one of `RESET_PIN`, `RESET_POR`, or `RESET_LOW_POWER_WAKE`, but unconfirmed)
3. Whether `AON_GPI1` (section 4.2) is actually a general-purpose GPIO accessible from software on M4 or M55, or whether it is AON-hardware-logic-only and inaccessible
4. Whether `AON_GPO1` (section 4.4) driving the power-switching transistor is actually linked to the `aon_event_gpio` driver's event configuration, or whether it is a completely static circuit
5. Whether the RTC (`U26 BU9873NUX`) alarm and PIR sensor (`PIR_INT`) are actually populated/connected on this board, or whether they exist in the schematic but may be unpopulated depending on the RDK's assembly option (population status cannot be determined from the schematic alone)
6. Since the M55 watchdog (`wdog0`) has `status = "disabled"` in the base dtsi, this project has not yet tried enabling it via `okay` in an actual board overlay — first attempted in Lab 02
7. Beyond `WDT_SYNA_DEFAULT_TIMEOUT_MAX` (default 1000ms), the actually configurable timeout range, and the measured time between "first expiration (callback)" and "second expiration (reset)"
8. As noted in the limitation in section 4.1, this section's signal-connection relationships are a first-pass, text-extraction-based confirmation; re-checking the schematic drawing pixel-by-pixel or verifying continuity with a multimeter on the actual board has not yet been done.

## 8. Next Lab Preview (Lab 01)

Since `CONFIG_PM` is not implemented (section 3.1), Lab 01 establishes a baseline using only the automatic WFI entry that Zephyr's kernel provides by default via the idle thread. M4 stays powered on at all times running a fixed workload (heartbeat), while only M55 is toggled — via an onboard button (SW8) — between ACTIVE (a mode where the CPU stays continuously ready so WFI never executes) and IDLE (a mode where it waits via `k_msleep()` between short tasks, so the idle thread executes WFI), while the current difference is measured with a multimeter.

# Lab 04 — Low-Power-Wake Exploration: the SW7 Button and RESET_LOW_POWER_WAKE

**Status: hardware-confirmed, finalized (2026-09-21)** — the full-power-cycle-then-SW7 test is left as optional (see "Running the lab and checking results" below) and this lab is closed out without it.

## Learning goals

- This is the most **exploratory (experimental)** lab in the curriculum — there is no predetermined right answer; the conclusion is only as complete as what hardware testing actually turns up.
- Directly verify, on hardware, the relationship between the `RESET_LOW_POWER_WAKE` category of `hwinfo_get_reset_cause()` (confirmed in the Lab 00 SDK/schematic review) and the physical "Wake-up" button (presumed to be `SW7`)/`PMU_EN` path identified in the schematic.
- Compare against Lab 02's already-established result that "the System Reset button always reports `RESET_POR`" to see whether `SW7` (a separate physical button) reports something different.
- Learn how to read a **raw register value** directly to check for undocumented bits outside the categories `hwinfo` names (read-only — nothing is ever written).
- The fact that success is not guaranteed here is itself part of the lesson — in hardware debugging, "we didn't find it" is also a valid, honestly-reported result.

## Connection to previous labs

Lab 00-03 already established three kinds of "wake" — Lab 01 (WFI, resumed by a timer), Lab 02 (watchdog reset, restarted from scratch), Lab 03 (mbox event, reacted to live with no reset). This lab looks for a fourth candidate: **"a different kind of wake, triggered by the M4/AON domain."**

In Lab 00's SDK source review, the mere existence of the `RESET_LOW_POWER_WAKE` category among the five `hwinfo_get_reset_cause()` distinguishes (`RESET_PIN`/`RESET_SOFTWARE`/`RESET_WATCHDOG`/`RESET_POR`/`RESET_LOW_POWER_WAKE`) was the key evidence that "an official path for M4/AON to wake M55 actually exists." However, this SDK has no API/Kconfig that lets an application actively trigger this from software — so this lab is one of **physical observation, not software reproduction**.

The schematic review (2026-09-21) identified the button in question (`SW7`, in the "RTC, Wake-up" section) as converging, together with the RTC interrupt (`RTC_INTn`) and the PIR sensor interrupt (`PIR_INT`), at an `EXT_INT` point that drives the `SR110_PMU_EN` signal — suggesting this is a physical-level mechanism that **(re-)activates the PMU itself**, entirely distinct from the "software reset of an already-powered SoC" (watchdog) covered in Lab 02.

## Hardware setup

- Keep both the M55 and M4 consoles open.
- This lab deals with a **PMU-level hardware button** not exposed to devicetree, so there is no overlay or wiring change at all — use the board as-is.
- Do not confuse the schematic's `SW7` ("Wake-up") with the System Reset button already covered in Lab 02 (a separate physical button). If the board has multiple buttons, it's recommended to first confirm exactly which one is `SW7` by cross-checking the silkscreen label against the schematic.
- If possible, it's worth setting up an environment where you can also try fully power-cycling the board (e.g. unplugging the USB cable) and then pressing `SW7` to see whether it really "wakes" (power comes back on) — this is optional.

## Devicetree overlay

**This lab has no overlay.** `SW7` is a PMU-level button not exposed as a node anywhere in devicetree (per the Lab 00 review), so there is no devicetree entry to turn on or off. This lab also doesn't touch any of the peripherals earlier labs used (`wdog0`/`i2c1`/`mbox`, etc.).

## Code structure

### M55 (`lab/src/main.c`)

This lab's code doesn't generate a trigger — it's an **observation tool**. It has three parts.

1. **Raw register dump** — `dump_raw_aon_por_rst(label)`: reads the same shared register `AON_POR_RST` (`0x50350038`, M4 = bit8, M55 = bit9 — confirmed in the Lab 00 SDK review) that `hwinfo_get_reset_cause()` decodes internally, via `sys_read32()`, **read-only**, and prints the raw 32-bit value as-is, including bits the hwinfo API doesn't name. Nothing is written, so this is safe.
2. **Standard reset-cause decoding** — reuses the `print_and_clear_reset_cause()` pattern established in Lab 02 as-is (decoding `RESET_PIN`/`RESET_SOFTWARE`/`RESET_WATCHDOG`/`RESET_POR`/`RESET_LOW_POWER_WAKE` + `hwinfo_clear_reset_cause()`). The `boot_local_counter` (cold-boot marker) is kept the same way too.
3. **Pre-clear / post-clear comparison** (added reflecting the 2026-09-21 first hardware result): the raw register is dumped twice — **before** and **right after** the `hwinfo_clear_reset_cause()` call. This is to distinguish whether `hwinfo_clear_reset_cause()` really writes to this hardware register, or only resets an internal software cache (variable) without touching the actual register (a common driver pattern). If the post-clear value equals the pre-clear value, it means this register is not cleared by the clear API.
4. **Live polling in the heartbeat loop** (added 2026-09-21): the original version read the register only once, at boot. The first hardware test result — "no reboot happened even while pressing SW7 with the board powered on" — couldn't rule out the possibility that **a register bit changed quietly, without any reboot**. So the raw value is now also printed via `sys_read32()` on every tick of the 2-second heartbeat loop — regardless of whether a reboot happens, you can check immediately, on the very next heartbeat line, whether the value changed right after pressing SW7. It's read-only, so it's safe to repeat indefinitely.

### M4 (`lab/remote/src/main.c`)

The same 1-second heartbeat as Lab 01/02, unchanged. It again serves as a control group — if the `SW7` trigger affects the whole SoC the way Lab 02's `RESET_SOC` did, M4's heartbeat would stop too; if only M55 is affected, M4 keeps running.

## prj.conf

`lab/prj.conf` (M55) — only the hwinfo API was added:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_HWINFO=y
```

`lab/remote/prj.conf` (M4) — identical to previous labs, unchanged.

## How to build

Same approach as previous labs.

```bash
# 1) Build M4 first.
west build -p always -b sr100_rdk/sr100/m4 -d m4 04_low_power_wake/lab/remote

# 2) Build M55, packaging the M4 image together with it.
west build -p always -b sr100_rdk/sr100/m55 -d m55 04_low_power_wake/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

## Running the lab and checking results

**Hardware result (2026-09-21) — while the SoC is powered on, SW7 produces no observable effect whatsoever:**

First test (reboot observation only): with the board running normally, pressing `SW7` produced no change at all on either the M55 or M4 console — the heartbeat kept ticking uninterrupted, and there was no reboot.

Second test (after adding live register polling, to double-check): "no reboot happened" alone couldn't tell us whether "a register bit quietly changed," so the code was enhanced to also print the `AON_POR_RST` raw value on every heartbeat tick (2 s). Pressing `SW7` repeatedly under this setup gave:

```
[boot] raw AON_POR_RST (0x50350038) = 0x00000fff (read-only, pre-clear)
[boot] reset cause bitmask = 0x00000008 -> RESET_POR
[boot] raw AON_POR_RST (0x50350038) = 0x00000fff (read-only, post-clear)
=== Lab 04 (M55): Low-Power-Wake / SW7 exploration ===
[main] heartbeat tick=0, uptime=60 ms, raw AON_POR_RST=0x00000fff
[main] heartbeat tick=1, uptime=2063 ms, raw AON_POR_RST=0x00000fff
...
[main] heartbeat tick=13, uptime=26101 ms, raw AON_POR_RST=0x00000fff
```

Even while continuously pressing `SW7` across 13 ticks (about 26 seconds), the raw value never changed by a single bit from `0x00000fff`. **This conclusively establishes that pressing SW7 while the SoC is already powered on causes neither a reboot nor any register bit change** — live polling ruled out the "quiet bit change" possibility that reboot observation alone could have missed.

Also confirmed: **the post-clear value (`0x00000fff`) was exactly identical to the pre-clear value.** In other words, `hwinfo_clear_reset_cause()` does not actually clear this raw `AON_POR_RST` register — the driver evidently only resets an internal software cache (variable) and never touches the hardware register itself. (This is also consistent with the possibility that this register is a "latch" type that only clears on a genuine POR — see the TBD section below.)

**Why this result is expected — `SW7`/`PMU_EN` and the scope of this SDK's Power Mode implementation (summarized 2026-09-21):**

If `SW7` really drives `PMU_EN` (a signal that (re-)activates SR110's PMU), then this button was likely designed, from the start, not as a signal aimed at "the SoC while it's already powered on and running," but as one meant to **turn back on (wake) a PMU that is OFF or that has entered a HW-level low power/ultra-low-power state**. In other words, the hardware result above ("SW7 has no effect while the SoC is powered on") should be reinterpreted not as a new anomaly, but as **a result that's consistent with — and should have been expected from — the very definition of `PMU_EN`** — re-asserting the same signal on a PMU that's already enabled has nothing left to change.

There is, however, one important constraint that defines the scope of this lab. As already confirmed in Lab 00's SDK source review (master doc section 1-2, item 1), **this SDK version has no `CONFIG_PM` at all, and there is not a single line of `pm_state_set`/`PM_STATE_*`-related code anywhere in the SDK.** That means there is no API/Kconfig in this SDK for software to actively put SR110 into an actual HW-level low power or ultra-low-power mode. To fully verify, on hardware, that `SW7` "meaningfully" works as `PMU_EN`, SR110 would first need to be put into that HW power mode — and the entry path itself simply doesn't exist in this SDK version. This is exactly why this lab was bound, from the outset, to have the exploratory character of "the conclusion only goes as far as what was actually found."

**This lab is closed out here.** Fully power-cycling the board and then pressing `SW7` to see whether power comes back on would still be a hardware-meaningful check (confirming the `PMU_EN` wiring itself), but as explained above, even a success there would only demonstrate **the cold-power-on capability of "SW7 can turn a fully dead PMU back on"** — not the true meaning of wake-up, i.e. "SR110 waking via SW7 from a HW-level low power mode commanded by software." Verifying the latter would first require a low-power-mode entry API that this SDK doesn't have. Judging that this doesn't affect this curriculum's conclusions, it's left untested and the lab moves on to the next one (Lab 05). (If it's tried later when the environment allows, the result can be added to this document's TBD section.)

## Items to verify (TBD)

- ~~Whether a reboot/register change occurs when SW7 is pressed while the SoC is powered on~~ — **confirmed (2026-09-21)**: no reboot; the raw `AON_POR_RST` value did not change by a single bit even across 13 ticks (~26 s) of continuous pressing (fixed at `0x00000fff`). The final conclusion is that, while the SoC is already powered on, none of these software observation channels can detect any effect from SW7 at all.
- ~~Whether `hwinfo_clear_reset_cause()` actually clears the raw register~~ — **confirmed (2026-09-21)**: it does not. The post-clear value was identical to the pre-clear value (`0x00000fff`). This API appears to only reset a software cache, while the hardware register is presumed to stay latched until an actual POR (unconfirmed — this remains an assumption until directly re-verified by forcing a POR with a debugger).
- The reason the value `0x00000fff` itself (all 12 low bits set) doesn't cleanly interpret as "1 bit = 1 trigger" is presumed to be that a genuine POR (power application) sets several of this register's monitored reset-source flags all at once — confirming this would require this register's bit-definition documentation, which the SDK does not include.
- **(Optional, not required for the curriculum) The lab was closed out without testing whether power comes back on after fully power-cycling the board and pressing SW7** — even a success there would demonstrate a "cold power-on" capability, not "wake-up from a HW low power mode," as explained above, and it doesn't affect this curriculum's final conclusions.
- **This SDK has no API/Kconfig at all for putting SR110 into a HW-level low power/ultra-low-power mode** (no `CONFIG_PM`, master doc section 1-2 item 1) — fully verifying the "wake-up" function of `SW7`/`PMU_EN` in its true sense would require this entry path first, which is out of scope for this curriculum (and this SDK version).
- Exactly which physical button on the board `SW7` is may need reconfirming by cross-checking the silkscreen label against the schematic (the Lab 00 review is a first-pass, text-extraction-based check and isn't exhaustive).
- Beyond `AON_POR_RST`, items on the master doc's section 1-4 TBD list — the `AON_CONFIG` register, `AON_GPI1` (GPIO3), `AON_GPO1` power switching, etc. — were not covered in this lab; items needing additional instrumentation like an oscilloscope are left out of this codebase's scope.
- Whether the RTC (`U26 BU9873NUX`) / PIR sensor are actually populated on this real RDK board is itself unconfirmed, so even if the power-cycle-then-SW7 test succeeds, it may be hard to tell whether that's "the button alone" or "the whole `EXT_INT` path shared by the button + RTC + PIR."

## Preview of the next lab

That `SW7` produces no software-observable effect while the board is powered on is this lab's primary conclusion, and it is reinterpreted as consistent with the understanding that `SW7` acts as `PMU_EN` (see the explanation above). Even if a retry after fully power-cycling the board succeeds, that alone would not lead to the conclusion that "this SDK supports wake-up from a HW low power mode" — the `PMU_EN` wiring itself would be confirmed, but this SDK simply has no API to enter such a mode in the first place (master doc section 1-2 item 1). Lab 05 (Synthesis: a reset-cause-based design guide) synthesizes these conclusions into the honest final statement that "the only wake categories this SDK/board combination has software-reproduced and verified are WFI (Lab 01), watchdog reset (Lab 02), and mbox events (Lab 03); the `RESET_LOW_POWER_WAKE` and `SW7`/`PMU_EN` path is confirmed to exist at the hardware level (as an `hwinfo` API category, and the `EXT_INT` -> `PMU_EN` path seen in the schematic), but this SDK version has no entry point for software to actively drive it, so full hardware verification was not reached."

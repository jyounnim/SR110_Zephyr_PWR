# Lab 02 Troubleshooting

## Issue 1 — `WDT_FLAG_RESET_NONE` Is Not "Stops in an Infinite Loop" but "Only the Callback Keeps Repeating" (2026-09-21)

### Symptom

Based on Lab 00's source review, Step 1 of Lab 02 was written expecting that setting `WDT_FLAG_RESET_NONE` and stopping the feed would cause the callback to fire once, after which the driver would internally spin in an infinite loop and the system would halt completely. What was observed on hardware was different.

```
[main] still running, 120197 ms since last feed (callback_fired=1)
[main] still running, 120450 ms since last feed (callback_fired=1)
>> [WDT] callback fired on channel 0 -- NOT feeding, watching what happens next
>> [WDT] RESET_NONE mode: no SoC reset should occur -- expect the driver to hang forever instead
[main] still running, 120703 ms since last feed (callback_fired=1)
...
[main] still running, 122474 ms since last feed (callback_fired=1)
>> [WDT] callback fired on channel 0 -- NOT feeding, watching what happens next
>> [WDT] RESET_NONE mode: no SoC reset should occur -- expect the driver to hang forever instead
[main] still running, 122727 ms since last feed (callback_fired=1)
...
```

The callback does not fire once and stop — it **keeps firing again at a period that almost exactly matches `WDT_TIMEOUT_MS` (2000ms)** (measuring the gap between callback occurrences in the log gives roughly 2000–2024ms). Meanwhile `main()`'s heartbeat log (`[main] still running, ...`) keeps printing every 250ms without a single gap — in other words, **there is no hang and no reset; the system keeps running completely normally the entire time.**

### Diagnosis — Lab 00's Source-Review Assumption Disproven on Hardware

At the time of Lab 00's review, we had concluded that "with `RESET_NONE`, not feeding causes an infinite loop at the second expiry" — but that was only an **assumption** made from reading the source, never verified on hardware in this project. This is the first time Lab 02 actually tested that assumption on hardware, and the result differed from the assumption.

The interpretation that best explains the hardware result is this: this driver (at least when a callback is registered and `RESET_NONE` is set) appears to **call the callback and then automatically re-load (re-arm) the hardware counter** on every timeout expiry — regardless of whether the application ever calls `wdt_feed()`. As a result, a watchdog configured with `RESET_NONE` effectively behaves like "a software timer that repeatedly calls a callback on a fixed period, without ever causing a reset or a hang." Given that re-arming the counter as part of clearing the interrupt is a common and reasonable watchdog hardware/driver design pattern, this is actually a more common and sensible design than "stops in an infinite loop."

**Important implication**: on this SDK, `WDT_FLAG_RESET_NONE` should only be used for pure notification/monitoring purposes — "get notified on timeout, but there's no real safety net (reset)." Since forgetting to feed it does not halt or restart the system, **`WDT_FLAG_RESET_SOC` must be used if a real fault-recovery (escape-the-hang) safety net is needed.**

### Actions Taken

1. Added a `callback_count` counter to `main.c`'s callback function, so the repeated calling of the callback is clearly visible in the log alone.
2. Corrected the guidance message inside the callback from "a hang is expected" to "this callback is expected to keep repeating."
3. Updated the file's top design notes and the reference doc (`02_watchdog_reset_kr.md`)'s "Connection to Lab 00/01," "Code Structure," and "Step 1" sections to match this hardware result.
4. **Whether `WDT_FLAG_RESET_SOC` actually causes a reset had not yet been confirmed at this point** — since the prior assumption about `RESET_NONE` had already turned out to be wrong once, the assumption of "a reset at roughly 2x the timeout" for `RESET_SOC` is treated only as a hypothesis until verified. This document would be updated once Step 2's hardware test results came in.

### Remaining TBD

- The exact mechanism (driver code path) by which the counter auto re-arms after the callback fires cannot be pinned down without re-reading the source — this project does not dig further into it given this hardware result.
- It cannot be ruled out that the same auto re-arming happens under `WDT_FLAG_RESET_SOC` too, meaning the reset itself might never occur. If so, this lab's original learning objective ("does stopping the feed really cause a reset?") could end up being reframed as "on this SDK, only `RESET_SOC` can be used as a reset safety net."


## Issue 2 — `WDT_FLAG_RESET_SOC` Halts the Whole SoC After the Callback and Never Reboots on Its Own (2026-09-21)

### Symptom

In the Step 2 test (`WDT_TEST_USE_RESET_SOC=1`), the callback fired normally once, but no further logs appeared afterward. The log provided cuts off right at the callback message:

```
*** Booting Zephyr OS build v4.4.1 ***
...
[main] watchdog armed: timeout=2000 ms, channel=0
[main] fed watchdog (1/5), uptime=... ms
...
[main] fed watchdog (5/5), uptime=... ms
[main] STOPPING FEED NOW at uptime=... ms -- watching for timeout/reset
[main] still running, ... ms since last feed (callback_fired=0)
...
>> [WDT] callback fired on channel 0 -- NOT feeding, watching what happens next
>> [WDT] RESET_SOC mode: a real SoC reset MAY follow around 2000 ms from here -- not yet confirmed on this SDK
```

(No further logs — the reboot banner (`*** Booting Zephyr OS ***`) never reappeared)

Two additional facts confirmed afterward:

1. **Pressing the physical reset button causes a normal reboot** — meaning the board itself, and the debugger/UART connection, are not fully dead; the system is still able to accept a reset normally.
2. **M4's log also halts during the frozen state** — not just the M55 console, but the heartbeat of M4 (`lab/remote/src/main.c`), an entirely separate core logging over its own separate UART, stops at the exact same moment.

### Diagnosis — the "Local Reset to M55 Only" Assumption Disproven on Hardware

The original design intent across Lab 00/01 and this document was that "`WDT_FLAG_RESET_SOC` resets only the M55 core, and M4, being Always-On, is unaffected" — `lab/remote/src/main.c` was even written specifically to verify that assumption (the idea being that M4 continuing to tick would prove only M55 had reset). The hardware result is the exact opposite: **M55 and M4 halt simultaneously and completely** after the callback, suggesting this is not a reset local to M55 but **a reset attempt targeting the whole SoC (both cores)**.

More importantly, this halted state **does not lead to a reboot on its own**. If this had been a SoC reset completing normally, both cores should have printed a boot banner again shortly after — but in practice, the system stayed halted indefinitely until the physical reset button was pressed. This can be explained by one of two interpretations:

- **(A) The reset was "latched/stuck"**: the watchdog asserted/latched a reset signal, but that reset sequence never completed at the hardware level and got stuck waiting — and the physical reset pin being pressed (combined with, or independently of, the already-latched reset request) is what finally completed the reset.
- **(B) The reset effectively had no lasting effect at all**: the watchdog's SoC reset attempt on this SDK/board combination didn't actually complete anything — it just halted the system — and the physical reset button performed a fully independent, normal reset sequence that recovered it.

The only clue that could distinguish these two interpretations is **what bitmask `hwinfo_get_reset_cause()` reports on the boot that follows pressing the physical reset button**. If the `RESET_WATCHDOG` bit appears alongside it, that leans toward (A) (the watchdog reset request survived in some form and got recorded as this boot's cause); if only `RESET_PIN` appears with no `RESET_WATCHDOG` at all, that leans toward (B) (the watchdog reset attempt left no trace, and this boot is purely the result of the pin reset). This information had not yet been confirmed at this point (see Remaining TBD).

One relevant piece of background: this driver rejects `WDT_FLAG_RESET_CPU_CORE` (a per-core reset) with `-EINVAL` from the start, supporting only `WDT_FLAG_RESET_NONE` and `WDT_FLAG_RESET_SOC`. This hardware result is consistent with that design — the fact that there was never even an option to reset "just one core" suggests this watchdog hardware was designed from the ground up to only be able to trigger a reset at the whole-SoC level, and `RESET_SOC` targeting "the entire SoC," exactly as its name says, is actually the natural outcome. However, whether the fact that this reset "does not complete on its own" is specific behavior of this particular watchdog, or a problem unique to this board/SDK combination, remains unknown.

**An important correction affecting the whole curriculum**: the claim repeated since Lab 00 — that "M4 is Always-On and unaffected by M55's power-state changes" — **does not hold**, at least in the situation where `WDT_FLAG_RESET_SOC` is triggered. It remains true that M4 keeps running independently of M55's IDLE/WFI (confirmed in Lab 00/01), but M4 also halts the moment M55's watchdog attempts a SoC reset. When designing future IPC (Lab 03) or Low-Power-Wake related labs, "M4 is always alive" should not be reused as a blanket assumption — the kind of M55 state change (power mode vs. reset) needs to be distinguished explicitly.

### Hardware Result Update — the Reset Cause After a Physical Reset Is Neither `RESET_WATCHDOG` nor `RESET_PIN`, but `RESET_POR` (2026-09-21)

The boot log right after pressing the physical reset button from the halted state:

```
>> [WDT] callback fired on channel 0 (call #1) -- NOT feeding, watching what happens next
>> [WDT] RESET_SOC mode: a real SoC reset MAY follow around 2000 ms from here -- not yet confirmed on this SDK
*** Booting Zephyr OS build v4.4.1 ***
[boot] reset cause bitmask = 0x00000008 -> RESET_POR
=== Lab 02: M55 Watchdog (wdog0) Reset Test ===
Build variant: WDT_TEST_USE_RESET_SOC=1 (WDT_FLAG_RESET_SOC)
[boot] boot_local_counter = 12345 (always reinitializes to 12345 on a cold boot -- ...)
[main] watchdog armed: timeout=2000 ms, channel=0
```

The two hypotheses (A)/(B) above were both framed around "does `RESET_WATCHDOG` appear, or only `RESET_PIN`?" — but in fact **neither appeared; `RESET_POR` (Power-On-Reset) did.** This was a third, unanticipated result, and it implies two things at once:

1. **The watchdog's SoC reset attempt left absolutely no trace in this boot's reported cause** — since the `RESET_WATCHDOG` bit is completely absent, hypothesis (A) ("the reset was latched and then completed together with the physical reset") is ruled out. This lines up exactly with the hypothesis raised by the user — **"it seems like the watchdog reset isn't getting set up (at the hardware level)"** — it's likely a driver bug where passing `WDT_FLAG_RESET_SOC` to `wdt_install_timeout()` never actually programs the real reset-enable register bit.
2. **However, this result alone cannot confirm hypothesis (B) either** ("the physical reset recovered it completely independently") — the fact that `RESET_POR` appeared instead of `RESET_PIN` raises the possibility that this board's physical "System Reset" button is not a simple pin-level reset at all, but is designed to trigger **a full power-on-reset (POR) sequence that drops all the way down to the PMU/power rail and back up** (consistent with the fact, confirmed in section 1-2 ⑤, that "System Reset" and "Wake-up" are separate buttons, and with the existence of the PMU_EN-based cold power-on path). If this button **always** reports `RESET_POR` regardless of the watchdog test, then this observation was never information that could distinguish (A)/(B) in the first place.

**Baseline test result (2026-09-21) — final conclusion**: pressing the same physical reset button during completely normal operation, unrelated to this lab, produced the exact same `reset cause bitmask = 0x00000008 -> RESET_POR`. In other words, **this board's physical "System Reset" button always reports `RESET_POR`, regardless of whether a watchdog was involved** (presumed to be in the same family as the PMU_EN-based cold power-on path from section 1-2 ⑤ — it appears to trigger a reset that drops down to the PMU and back, not a simple pin signal).

This confirms that the diagnostic approach of using `hwinfo_get_reset_cause()` to distinguish hypotheses (A)/(B) is **a dead end**: as long as recovery goes through this button, the reset-cause register always gets overwritten with POR regardless of whatever the watchdog did beforehand, so there is no way, with this method, to tell whether the watchdog's SoC reset attempt was actually reflected in hardware. Neither (A) ("the reset was latched") nor (B) ("the watchdog reset left no trace") contradicts this observation, so this question is concluded to be unresolvable any further with this project's tools (multimeter, console logs). Going deeper would require observing the watchdog's registers directly with a debugger, or observing the reset line itself with an oscilloscope — both out of scope for this curriculum.

The hypothesis raised by the user — "it seems like the watchdog reset isn't getting set up at the hardware level" — remains a strong candidate, but cannot be confirmed within this project for the reasons above.

### Actions Taken

1. Updated the "Connection to Lab 00/01," "Hardware Setup," "Step 2," and "Remaining TBD" sections of `02_watchdog_reset_kr.md` to match this hardware result.
2. Corrected the top-of-file design notes in `lab/src/main.c` (M55) and `lab/remote/src/main.c` (M4) to match this result — in particular, spelling out that M4's original claim ("M55's watchdog reset proves it's a reset local to M55 alone") had turned out to be the exact opposite.
3. Corrected the M55 callback's `RESET_SOC`-branch log message from "a reset may follow (unconfirmed)" to "the whole SoC (M55+M4) will halt and needs a physical reset, since it doesn't recover on its own."

### Remaining TBD

- ~~The bitmask `hwinfo_get_reset_cause()` reports right after pressing the physical reset button~~ — **Confirmed (2026-09-21): always `RESET_POR` (`0x00000008`), confirmed via a baseline test to be a property of this button itself, unrelated to the watchdog.** As a result, `hwinfo_get_reset_cause()`-based diagnosis cannot distinguish (A)/(B) — see the section above for the final conclusion.
- Whether this "halts and never recovers on its own" behavior varies with the `WDT_TIMEOUT_MS` value (2000ms) or behaves differently at longer timeouts was not checked — this project will not investigate further within its current scope.
- Whether the reset-enable register bit is actually programmed when `WDT_FLAG_RESET_SOC` is passed to `wdt_install_timeout()`/`wdt_setup()` was concluded to be undeterminable from console logs and reset_cause alone (see the section above) — this would require observing the register directly with a debugger, which is out of scope for this project.
- **Final conclusion**: on this SDK/board combination, `WDT_FLAG_RESET_SOC` was **not confirmed** to be a self-completing safety-net reset — stopping the feed causes the callback to fire exactly once, but the system (M55 and M4 together) then halts and does not recover without physical intervention. The root cause (whether a hardware reset was actually triggered but failed to complete, or was never triggered at all) could not be determined with this project's tools. Adopting this watchdog as a safety net in an actual product would require separate confirmation from Synaptics based on this result.

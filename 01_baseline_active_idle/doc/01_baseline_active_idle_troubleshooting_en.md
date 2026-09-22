# Lab 01 Troubleshooting — Mode-Switch Messages Printing on Their Own, With No Input (2026-09-21)

## Symptom

On the M55 console, mode-switch/status-query confirmation messages were observed printing several times in quick succession, even though the user had not typed that many commands:

```
[IDLE] uptime=18062 ms -- sleeping 2000 ms (WFI expected)
>> ACTIVE mode: CPU stays ready, WFI never runs
>> IDLE mode: CPU sleeps via k_msleep()/WFI between bursts
>> IDLE mode: CPU sleeps via k_msleep()/WFI between bursts
>> IDLE mode: CPU sleeps via k_msleep()/WFI between bursts
>> Current mode: IDLE
>> Current mode: IDLE
>> ACTIVE mode: CPU stays ready, WFI never runs
```

## Diagnosis

The first version of this lab (`uart_cmd_task`) was structured to read one byte at a time from the console UART with `uart_poll_in()`, and **immediately** switch mode or print status whenever that byte was `'1'`/`'2'`/`'3'`. In other words, a command executed whenever one of these three bytes arrived on RX through any path, regardless of whether it was an actual keypress.

Looking at the observed log, the previously printed status message `[IDLE] uptime=18062 ms -- sleeping 2000 ms (WFI expected)` literally contains the digits `1`, `8`, `0`, `6`, `2`, `2`, `0`, `0`, `0`. The order in which the command-interpretable digits (`1`, `2`) appear among these matches the order of the mode-switch messages printed right after (ACTIVE → IDLE → IDLE). This strongly suggests that **M55's own console output (TX) was somehow looping back into its own receive path (RX), and the digits inside the log string were being misinterpreted as commands.**

Possible causes (in priority order, not yet pinned down on hardware — TBD):

1. The USB-UART converter/bridge used for the console may be operating in a TX/RX loopback or half-duplex echo mode (seen on some low-cost modules).
2. TX and RX pins may have been accidentally shorted together in the wiring connecting the J25 header to an external converter.
3. A terminal program's "Local echo" is normally display-only and not retransmitted to the device, but some terminal/setting combinations (e.g., a forced half-duplex setting) can actually retransmit it to the device, so this was not fully ruled out.

**Recommended procedure to pin down the root cause**: watch the console for over a minute without pressing any key. If `>> ACTIVE mode`/`>> IDLE mode`/`>> Current mode` messages keep appearing on their own during that time, the diagnosis above is correct; conversely, if no extra messages appear (only the `[IDLE] uptime=...` status log every 2 seconds), then the earlier observation was simply the user's own repeated input, not this issue.

## Fix

Since the root cause (whether it's hardware loopback) is not yet confirmed, the issue was first filtered out at the software level.

`uart_cmd_task` was changed from "execute immediately on receiving one digit byte" to **"hold a received digit as pending, and only commit and execute the command once Enter (`\r` or `\n`) follows right after."**

```c
if (c == '\r' || c == '\n') {
        switch (pending_cmd) {
        case '1': /* ... switch to ACTIVE ... */ break;
        case '2': /* ... switch to IDLE ... */ break;
        case '3': /* ... print status ... */ break;
        default: break;
        }
        pending_cmd = 0;
} else if (c == '1' || c == '2' || c == '3') {
        pending_cmd = c;
}
```

This works regardless of the root cause because none of this lab's log output strings ever have a digit immediately followed by a carriage return/newline (e.g., the `2` in `"...18062 ms..."` is followed by a space, not a newline). By contrast, when a person actually types `1` on the console and presses Enter, it produces exactly the pattern "digit immediately followed by CR/LF." So this fix effectively filters out digits arriving via loopback while still accepting genuine user input.

## Remaining TBD (Issue 1)

- The exact cause of the loopback (converter hardware vs. wiring vs. terminal settings) has not yet been pinned down. Please run the "recommended procedure" above to reproduce/rule it out, and this document will be updated with the result.
- If the actual cause turns out to be hardware loopback, the same problem could recur on the M4 console once a later lab covers IPC (Lab 03), and the same fix would need to be applied there as well.

---

# Issue 2 — Console Commands Stop Working Entirely After Entering ACTIVE Mode (2026-09-21)

## Symptom

After applying the fix from Issue 1 above (digit+Enter commit), re-testing showed the spurious output (messages printing on their own) was gone. But a few commands right after boot were accepted normally and switched to ACTIVE mode, and after that, typing any of `1`/`2`/`3` and pressing Enter had no effect at all — only the `[ACTIVE] uptime=...` log kept printing every 2 seconds, permanently stuck.

```
Boot mode: IDLE
[IDLE] uptime=39 ms -- sleeping 2000 ms (WFI expected)
>> ACTIVE mode: CPU stays ready, WFI never runs
>> IDLE mode: CPU sleeps via k_msleep()/WFI between bursts
>> ACTIVE mode: CPU stays ready, WFI never runs
[ACTIVE] uptime=2042 ms -- CPU never idles
[ACTIVE] uptime=4042 ms -- CPU never idles
... (stuck in ACTIVE forever, no command has any effect afterward)
```

## Diagnosis

The root cause was a thread-priority design mistake.

- `uart_cmd_task` was created via `K_THREAD_DEFINE` at priority **5**.
- `main()` (the thread running the ACTIVE/IDLE workload) had no priority explicitly set, so it kept Zephyr's default (`CONFIG_MAIN_THREAD_PRIORITY`, a **numerically smaller — i.e., higher — priority** than 5 on this board).
- ACTIVE mode's `main()` loop repeats only `k_yield()` and never blocks. But per Zephyr's documentation, `k_yield()` only yields the CPU to **another ready thread of the same priority as the current thread** — it does not yield to lower-priority threads.
- In other words, once `main()` (at higher priority than `uart_cmd_task`) entered ACTIVE mode, since it never blocks and `k_yield()` never hands off to the lower-priority `uart_cmd_task`, **`uart_cmd_task` never gets a chance to be scheduled again.** As a result, the code polling the console UART itself never ran, so no further keypress had any effect.
- Why IDLE mode wasn't a problem: because `main()` genuinely blocks via `k_msleep(2000)`, `uart_cmd_task` — the only ready thread during that gap — gets scheduled normally regardless of priority. So "entering `1` in IDLE to switch to ACTIVE" always succeeded, but once in ACTIVE, there was never any way back out (the two extra ACTIVE→IDLE→ACTIVE transitions seen right after boot in the log were only accepted by chance, because `main()` was still in its first IDLE sleep window at that point).

## Fix

`uart_cmd_task`'s priority was re-set to always be higher (a smaller number) than `main()`'s, and `main()` explicitly lowered its own priority at startup via `k_thread_priority_set()`.

```c
#define UART_CMD_THREAD_PRIORITY 1   /* higher priority (smaller number) */
#define WORKLOAD_THREAD_PRIORITY 10  /* lower priority */

K_THREAD_DEFINE(uart_cmd_tid, 1024, uart_cmd_task, NULL, NULL, NULL,
                 UART_CMD_THREAD_PRIORITY, 0, 0);

int main(void)
{
        k_thread_priority_set(k_current_get(), WORKLOAD_THREAD_PRIORITY);
        ...
}
```

With this, every time `uart_cmd_task` wakes from its own `k_msleep(10)`, its higher priority **guarantees it preempts** the lower-priority `main()`'s `k_yield()` loop, giving it a chance to poll UART. This preserves the property that `main()` never blocks in ACTIVE mode (i.e., the idle thread/WFI never runs — this lab's core purpose) while still guaranteeing command responsiveness.

## Lesson (applies to other labs too)

**Whenever the code has a "non-blocking busy loop," every other thread that must interact with it (console command handling, heartbeats, IPC callback workers, etc.) must have a strictly higher priority than it.** Always remember that `k_yield()` "yields the CPU only to threads of the same priority," not to lower-priority ones. Whenever a non-blocking loop (e.g., a polling-based workload) is added in a later lab, explicitly check this priority relationship every time.

---

# Issue 3 — Even After the Issue 2 Fix, Only `1` Keeps Registering in ACTIVE Mode While `2`/`3` Never Respond (Finally Resolved by Discarding the Console Interface Entirely, 2026-09-21)

## Symptom

After applying the Issue 2 priority fix and re-testing, the "stuck in ACTIVE" problem was gone (console commands were still being processed), but the following was observed: **only the `>> ACTIVE mode: ...` confirmation message kept repeating, and entering `2` or `3` never produced a `>> IDLE mode` or `>> Current mode` message at all.**

```
[ACTIVE] uptime=34907 ms -- CPU never idles
>> ACTIVE mode: CPU stays ready, WFI never runs
>> ACTIVE mode: CPU stays ready, WFI never runs
[ACTIVE] uptime=36907 ms -- CPU never idles
>> ACTIVE mode: CPU stays ready, WFI never runs
...
```

## Assessment at This Point

What stands out in this log is that none of the preceding status-log strings (`"34907"`, `"CPU never idles"`, etc.) contain the digit `1`. So this is different from the "digits inside a log string looping back as input" pattern covered in Issue 1. Also, if this were the priority problem covered in Issue 2, no command at all (1, 2, or 3) should have registered — but `1` kept registering, so it isn't the priority issue either.

At this point, two possibilities could not be distinguished:

1. The `2`/`3` keys were actually pressed, but those bytes themselves never reached M55's UART RX at all (a wiring/converter/terminal-program key-mapping issue, etc.)
2. `1` was pressed repeatedly as a test, and `2`/`3` either hadn't been tried yet, or were not actually transmitted for some other reason.

## Fix — Added Temporary Diagnostic Code

Rather than narrowing the cause by guesswork, temporary diagnostic code was added so `uart_cmd_task` **prints every raw byte it actually receives, as-is** (`main.c`, marked with a "TEMPORARY DIAGNOSTIC" comment).

```c
printk("[uart_cmd_task] rx raw byte: 0x%02x ('%c')\n",
       c, (c >= 0x20 && c < 0x7f) ? (char)c : '.');
```

This shows every byte arriving on RX, in hex and as a printable character, regardless of whether it's recognized as a command. This makes it possible to directly tell "does pressing 2/3 really produce no byte at all," versus "a byte does arrive, but it's not the expected `0x32` (`'2'`)/`0x33` (`'3'`)."

**Next verification step**: after rebuilding/reflashing, please report exactly how many `[uart_cmd_task] rx raw byte: ...` lines appear on the console (and with what values) when `2` is entered and Enter is pressed, so the cause can be pinned down. This diagnostic code will be removed once the cause is identified.

## Follow-up Diagnosis — M4 Cross-Talk Hypothesis Disproven on Hardware (2026-09-21)

After adding the temporary raw-byte diagnostic (above) and re-testing, the following was repeatedly observed:

```
[uart_cmd_task] rx raw byte: 0x5b ('[')
[uart_cmd_task] rx raw byte: 0x30 ('0')
[uart_cmd_task] rx raw byte: 0x5d (']')
[uart_cmd_task] rx raw byte: 0x5b ('[')
...
A whole string of the form "[0][INF][SYS ]:...Task vTaskDemo1" arriving on RX
```

This string did not match the format of any code in this project's Lab 01 (a FreeRTOS-style log tag, with a task name `vTaskDemo1`), and the fact that a newline follows immediately after the final `1` matched exactly the "only `1` keeps registering" symptom observed in Issue 3. This initially led to the hypothesis that **"M4 is still running old/factory FreeRTOS firmware that prints this string, and its output is somehow bleeding into M55's RX through the wiring,"** and a request was made to check the M4 console directly and unplug M4's UART adapter to see if the issue still reproduced with M55 alone.

**Result: this hypothesis was disproven on hardware.**

- The M4 console was completely clean — only the `=== Lab 01 (M4): Always-On heartbeat ===` banner and `[M4 heartbeat] tick=N, uptime=... ms` printed, with no trace of the suspected FreeRTOS log format.
- With M55 fully isolated from M4 and connected alone, the exact same garbage string still reproduced.

So the issue is unrelated to M4's wiring/firmware. Given grounds to suspect a **"bug in the M55-side console program itself (the PC-side serial monitor tool)"** that the user had encountered before, rather than debugging that console program itself this time (which is out of scope for this curriculum), the final decision was to **discard the UART console command interface entirely.**

## Final Fix — Replace Console Commands With the Onboard Button (SW8)

All UART command-processing code — `uart_cmd_task`, the temporary raw-byte diagnostic, and the Enter-commit logic — was removed. Instead, the onboard physical button **SW8 ("USER_BUTTON")**, already validated in this project's M4↔M55 IPC curriculum, is read directly on M55 and toggles ACTIVE/IDLE on each press (using Zephyr's `zephyr/input/input.h` input subsystem, `INPUT_CALLBACK_DEFINE()`).

This replacement also resolves the following at the same time:

- Mode switching no longer goes through the console UART's RX path at all, so whatever the root cause was, it's no longer affected by that console-program bug.
- The thread-priority issue from Issue 2 (`uart_cmd_task` vs. `main()`) is also resolved — the input-subsystem callback runs on the system workqueue thread (cooperative priority), so the ACTIVE mode busy loop can always be preempted without needing to separately lower `main()`'s priority.

However, SW8 is not a direct SoC GPIO — it sits behind the PCA6416A GPIO expander (`gpio_exp0`) on the I2C1 bus. Unlike the IPC curriculum, where M4's `gpio_exp0` has no `int-gpios` and so doesn't support interrupts (`-ENOTSUP`, worked around with `&buttons { polling-mode; };`), **that same curriculum already confirmed that M55's `gpio_exp0` does define `int-gpios`**, so interrupt-based operation was expected to work normally on M55 this time — however, this combination (M55 + `gpio_exp0` interrupt) is being validated on hardware for the first time in this project, so it is **TBD**. If the boot log shows `gpio_keys: interrupt configuration failed: -134`, add `polling-mode;` to the `&buttons` node (not the child `&user_button`) in the M55 overlay, the same way the IPC curriculum did:

```dts
&buttons {
	polling-mode;
};
```

Also, since M55 now needs to master I2C1, this lab's overlay configuration has been flipped from the previous version — see the "Devicetree Overlays" section of the updated `01_baseline_active_idle_en.md` for details.

**This issue is closed without the root cause (the console program's own bug) being confirmed** — since this curriculum's goal is power-management learning, rather than tracking down a bug in a PC-side tool, the design was reworked to not depend on that tool at all. The console program's exact defect is left as TBD.

---

# Issue 4 — After the v2 (SW8 Button) Redesign, Only the First Press After Boot (IDLE→ACTIVE) Registers, Nothing After That (2026-09-21, Resolved With polling-mode)

## Symptom

After flashing v2, which replaced console commands with the SW8 button:

```
Boot mode: IDLE
[IDLE] uptime=50 ms -- sleeping 2000 ms (WFI expected)
...
>> [SW8] ACTIVE mode: CPU stays ready, WFI never runs
[ACTIVE] uptime=14068 ms -- CPU never idles
[ACTIVE] uptime=16068 ms -- CPU never idles
... (pressing SW8 again never produces a `>> [SW8] IDLE mode` message; only [ACTIVE] logs continue)
```

The first button press after boot (IDLE→ACTIVE) registered correctly, but the next press (attempting to return ACTIVE→IDLE) had no effect at all.

## Diagnosis

The mode-toggle logic in `main.c` itself is completely symmetric between IDLE→ACTIVE and ACTIVE→IDLE (`if (mode == MODE_ACTIVE) { ... } else { ... }`), so there is no direction-dependent asymmetry in the application logic. Yet only one direction failing means the problem is happening before reaching the application — i.e., the `gpio_exp0` (PCA6416A) interrupt itself **fires only once and never re-arms after that.**

This is a common class of problem with buttons behind an I2C GPIO expander — the expander's `INT#` pin is only released (re-armed) once the input port register is read over I2C, and if this re-arming (debounce, then reconfiguring the interrupt for the next edge, etc.) doesn't line up exactly right, any transition after the second one simply never triggers an interrupt at all. This project's M4↔M55 IPC curriculum saw M4 unable to use interrupts at all on `gpio_exp0` due to missing `int-gpios` (worked around with `&buttons { polling-mode; };`) — the symptom here is different (M55 has `int-gpios` and does work at least once), but the **result is the same class of "I2C expander + interrupt-based gpio-keys" instability.**

## Fix

The same `polling-mode` was applied to the M55 overlay as well (`lab/boards/sr100_rdk_sr100_m55.overlay`):

```dts
&buttons {
	polling-mode;
};
```

Since this periodically polls the GPIO expander's registers instead of relying on interrupts, there is no room for a re-arming problem to occur on each transition. The default `debounce-interval-ms` (30 ms) is sufficient for this lab (already validated in the IPC curriculum).

**Next verification step**: after rebuilding (`-p always` recommended — since this is a devicetree change, the build-cache issue could recur; see section 11.1) and reflashing, please confirm ACTIVE↔IDLE toggles correctly every time across multiple consecutive SW8 presses.

## Remaining TBD

- The exact internal mechanism behind why interrupt mode worked "exactly once" (a failure to reschedule the debounce work item vs. an interrupt trigger-polarity configuration issue, etc.) was not pinned down — since it was worked around with polling-mode, this was not pursued further within this project's scope.

---

# Issue 5 — Same Symptom Recurs Even With polling-mode Applied: Only the First Transition (IDLE→ACTIVE) Registers, Nothing After (Thread-Priority Hypothesis — Disproven on Hardware, See Issue 6, 2026-09-21)

## Symptom

After applying the Issue 4 fix (`&buttons { polling-mode; };`) and re-testing, the exact same symptom reproduced — only the first button press after boot (IDLE→ACTIVE) registers, and pressing the button again afterward (attempting to return to IDLE) has no effect at all.

## Assessment at This Point

The fact that interrupt mode and polling-mode — which have entirely different hardware/re-arming mechanisms — reproduce the exact same symptom suggests the cause lies not in that lower layer (interrupt re-arming, poll period, etc.) but in **a higher-level piece of logic common to both modes.** The leading hypothesis was:

**The release (value=0) event may be getting dropped at the driver level.** The `input_gpio_keys` driver internally compares "the previously settled state" against "the newly read state," and only reports an event when they differ. If the driver is still reading "pressed" even after the button is released (e.g., because a subsequent I2C read on `gpio_exp0` fails and the old value is reused, or a polarity/debounce configuration issue), then the next actual press looks like "already pressed → pressed" from the driver's perspective — no state change, so no event fires at all. This is higher-level logic that could show up identically in either interrupt mode or polling-mode, and it matches the observed symptom exactly.

## Fix — Added Temporary Diagnostic Code

Rather than narrowing the cause by guesswork, temporary diagnostic code was added so `button_input_cb` prints **every raw input event** (type/code/value) unfiltered (`main.c`, marked with a "TEMPORARY DIAGNOSTIC" comment).

```c
printk("[button_input_cb] type=0x%02x code=0x%02x value=%d\n",
       evt->type, evt->code, evt->value);
```

**Next verification step**: after rebuilding/reflashing, please press and release SW8 several times (at least 3–4 times) and report whether a `[button_input_cb] ...` line appears every time, and if so, whether `value` correctly alternates between 1 and 0 on each press/release.

- If `[button_input_cb]` stops appearing entirely from the second press onward → the cause narrows to the `gpio_exp0`/I2C1 bus level failing to read the value change at all (e.g., I2C transaction failures).
- If the log does appear but `value` looks wrong (e.g., release never comes, or it's always the same value) → the cause narrows to a debounce/polarity configuration issue.

This diagnostic code will be removed once the cause is identified.

## Follow-up Diagnosis — Root Cause Pinned Down via Raw Event Log (2026-09-21)

After adding the diagnostic code and re-testing, a decisive log was captured:

```
[IDLE] uptime=8058 ms -- sleeping 2000 ms (WFI expected)
[button_input_cb] type=0x01 code=0x0b value=1
>> [SW8] ACTIVE mode: CPU stays ready, WFI never runs
[button_input_cb] type=0x01 code=0x0b value=0
[ACTIVE] uptime=10060 ms -- CPU never idles
[ACTIVE] uptime=12060 ms -- CPU never idles
... (no matter how many more times SW8 is pressed, not a single more [button_input_cb] line ever appears)
```

**Key observation**: both the initial press (`value=1`) and release (`value=0`) events were captured correctly — meaning the "dropped release event" hypothesis raised at the start of Issue 5 was wrong. The problem is precisely that the callback itself (not even the raw event) stops entirely **from the exact moment ACTIVE mode is entered.**

## Root Cause Identified

This lab's `button_input_cb` is invoked from the input subsystem's **workqueue thread** (not an ISR). It was initially assumed — and written into the code/docs as if true — that "the system workqueue runs at cooperative (negative) priority and therefore always preempts other threads." **This assumption was never actually verified, and it turned out to be wrong on this SDK.**

`main()` had not lowered its own priority, so it was running at Zephyr's default priority (preemptible, and equal to or higher than the workqueue's priority on this SDK). ACTIVE mode's `main()` loop repeats only `k_yield()` and never blocks, but `k_yield()` only hands the CPU to another ready thread **of the same priority** — never to one that is lower priority (or simply not equal). In other words, if the workqueue thread's priority was lower than (or simply not equal to) `main()`'s, then from the moment ACTIVE mode was entered, `main()`'s infinite `k_yield()` loop permanently starved the workqueue thread, so it never got the chance to poll/process SW8's GPIO value change at all. In IDLE mode, `main()` genuinely blocks via `k_msleep()`, so the workqueue gets scheduled normally — which is why the very first IDLE→ACTIVE transition after boot (both press and release) always worked correctly.

This is **exactly the same class of priority bug** that existed between `uart_cmd_task` and `main()` in this lab's v1 (console-command version) — see Issue 2. This time the counterpart just happened to be Zephyr's input-subsystem workqueue instead of a thread written by hand.

## Fix

Reverted to having `main()` explicitly lower its own priority at startup via `k_thread_priority_set()` (a larger number, i.e., a lower priority) — the same fix used in v1.

```c
#define WORKLOAD_THREAD_PRIORITY 10

int main(void)
{
        k_thread_priority_set(k_current_get(), WORKLOAD_THREAD_PRIORITY);
        ...
}
```

The exact default value of `CONFIG_SYSTEM_WORKQUEUE_PRIORITY` on this SDK was not confirmed (TBD) — instead, `main()` was reliably lowered enough that it always loses to the workqueue thread's ACTIVE busy loop, regardless of whatever the workqueue's actual priority is.

**Next verification step**: after rebuilding/reflashing, please confirm that `[button_input_cb]` keeps printing on every SW8 press even after entering ACTIVE mode, and that ACTIVE↔IDLE toggles correctly every time. Once confirmed, the diagnostic raw-event log (`printk("[button_input_cb] ...")`) will be removed in the next revision.

## Lesson (applies to other labs too)

**Whenever the code has a "non-blocking busy loop," you must explicitly confirm/guarantee that every other thread that must interact with it — not just hand-written threads, but also workqueue threads Zephyr uses internally — has a strictly higher priority than that busy loop.** The assumption that "the system workqueue is cooperative-priority by default" must not be trusted on this SDK without verification — this is the same lesson from Issue 2, applying equally to a workqueue.

---

# Issue 6 — The Issue 5 Priority Hypothesis Disproven on Hardware: Finally Resolved by Bypassing the gpio-keys Subsystem Entirely (2026-09-21)

## Symptom

Following the hypothesis from Issue 5 that "the workqueue thread is starved by `main()`'s ACTIVE busy loop," `main()`'s priority was explicitly lowered (`k_thread_priority_set()`). Re-testing, however, showed **the exact same symptom reproducing identically** — only the very first transition after boot (both press+release logged correctly) registers, and no matter how many more times SW8 is pressed afterward, not a single more `[button_input_cb]` line ever appears.

```
[IDLE] uptime=4053 ms -- sleeping 2000 ms (WFI expected)
[button_input_cb] type=0x01 code=0x0b value=1
>> [SW8] ACTIVE mode: CPU stays ready, WFI never runs
[button_input_cb] type=0x01 code=0x0b value=0
[ACTIVE] uptime=6055 ms -- CPU never idles
... (repeats forever, pressing SW8 has no effect)
```

## Diagnosis — Priority Hypothesis Disproven, Reconsidered on the User's Objection

The user raised three specific points: (1) if this really were a priority issue, it would already be a well-known pattern in the Zephyr community, so it's odd for it to be blocked this completely; (2) if it truly were a priority issue, simply using `k_msleep(1)` instead of `k_yield()` would work around it (since the thread would leave the ready queue entirely); (3) shouldn't key-event/interrupt handling sit above thread priority in the first place?

Reconsidering with these points in mind:

- (1) and (2) are theoretically sound, but the fact that explicitly lowering `main()`'s priority on real hardware reproduced the exact same symptom **directly disproves the priority hypothesis** by itself. If the workqueue thread really was being starved because it had a lower priority than `main()`, lowering `main()`'s priority further should necessarily have resolved it.
- (3) was the decisive point — **a genuine hardware interrupt (ISR) sits outside the thread-priority system and always wins, but `polling-mode`, by definition, doesn't use interrupts at all — it's the workqueue thread polling periodically.** What's more, even Issue 4's interrupt mode likely ended up doing the actual register read in the workqueue thread woken by the ISR anyway, since a blocking I2C transaction can't be done directly inside an ISR. In other words, whether interrupt mode or polling-mode, "the code that actually processes an SW8 press" was running in thread context (the workqueue) either way — but **this re-verification confirmed that thread was not starved due to priority.**

**Conclusion**: the cause narrows down not to thread priority, but to **a bug in this SDK's `gpio-keys`/`input_gpio_keys` driver's own polling/interrupt state machine** (the exact internal mechanism remains unidentified — without access to the source, no further guessing is done).

## Fix — Bypass the Zephyr gpio-keys/Input Subsystem, Poll GPIO Directly From the Application

Rather than continuing to debug this driver by guesswork, SW8 is now switched to being **read directly via the plain GPIO API (`gpio_pin_get_dt()`)**, using the same philosophy (self-implemented sample-and-debounce) this lab's v1 used for UART command processing.

- `main.c`: all use of `zephyr/input/input.h`/`INPUT_CALLBACK_DEFINE()` removed. Instead, the existing `gpios` property on the `user_button` devicetree node is fetched directly via `GPIO_DT_SPEC_GET(DT_NODELABEL(user_button), gpios)`, and a dedicated thread (`button_poll_task`, priority 1) polls every 20 ms with a simple self-implemented debounce that requires 3 consecutive identical readings before confirming a state change. `main()` lowers its own priority to 10, exactly as in v1, so `button_poll_task` always has priority over it (this priority arrangement is a pattern already validated on hardware in v1).
- `boards/sr100_rdk_sr100_m55.overlay`: `&buttons { polling-mode; };` removed (no longer needed since that driver isn't used at all anymore). `&i2c1`/`&gpio_exp0` remain enabled as before (still needed for direct GPIO access).
- `prj.conf`: `CONFIG_INPUT`/`CONFIG_INPUT_GPIO_KEYS` removed.
- Diagnostic printk statements (raw read errors, every state change) were kept, so that this time too it's possible to tell whether the cause is at the I2C/hardware level.

**This fix doubles as a diagnostic**: if this approach also stops after the first transition, the cause is not the gpio-keys driver but the I2C1/`gpio_exp0` hardware level (e.g., a bus lock-up); if it keeps working correctly, that confirms the bug was in the gpio-keys driver itself.

**Next verification step**: after rebuilding (`-p always` recommended — prj.conf/overlay changes) and reflashing, please press SW8 repeatedly, even in ACTIVE mode, and confirm that `[button_poll_task] stable state -> ...` prints every time and ACTIVE↔IDLE toggles correctly.

## Correction — the "Interrupt Re-Arm Failure" / "Workqueue Priority" Diagnoses From Issues 4/5 Are Withdrawn for Lack of Evidence

Both the "M55's `gpio_exp0` interrupt fails to re-arm" diagnosis from Issue 4 and the "the workqueue thread is starved due to priority" diagnosis from Issue 5 were **only hypotheses put forward to explain the symptom (only the first transition works) — neither was ever directly verified on hardware, and this priority fix's hardware disproof has removed the basis for either.** The most well-supported explanation at this point is an internal bug in this SDK's gpio-keys driver (the exact mechanism unidentified), and this fix (bypassing the driver) will provide the final confirmation.

---

# Issue 7 — v3's `button_poll_task` Contaminates the IDLE Measurement Itself: Redesigned to Remove the Thread and Integrate Polling Into main() (2026-09-21)

## Problem — Caught During Design Review, Before Hardware Testing

Before v3 was ever flashed to hardware, the user caught a decisive flaw while reviewing the v3 design: **"I agree with removing Zephyr's gpio-keys/input subsystem in favor of directly polling GPIO, but `button_poll_task` looping forever every 20 ms via `k_msleep(20)` runs exactly the same way in IDLE mode too — doesn't that mean the CPU keeps waking up every 20 ms even while in the IDLE state?"**

This is exactly right. v3's `button_poll_task` wakes up and runs on a fixed 20 ms period regardless of the `g_mode` value. So even though IDLE mode was meant to demonstrate "the CPU sleeps via WFI," this polling thread actually wakes the CPU every 20 ms, meaning **a multimeter can never see a clean WFI-sleep baseline current at all — only a current with a 20 ms-period wake spike superimposed on it every single time.** This is a design flaw that defeats the very purpose this lab exists for (measuring the ACTIVE vs. IDLE current baseline), and it is a completely separate issue from whether the gpio-keys driver has a bug.

## Fix — Remove the Dedicated Thread, Integrate Into main()'s Loop Differently Per Mode

In v4, the `button_poll_task` thread and its `K_THREAD_DEFINE()` were removed entirely, and button polling was absorbed into `main()`'s existing loop. Polling is handled differently per mode so that **neither mode's current profile is disturbed.**

- **IDLE**: right after the `k_msleep(IDLE_SLEEP_MS)` (2 seconds) that `main()` was already doing for its status log, the button is read exactly once while it's there. Since the CPU was already going to wake up at that point anyway, **this adds no extra wake events whatsoever.** No separate debounce was added either, because the 2-second sampling interval itself is far longer than mechanical button chatter (on the order of a few ms), so it's naturally filtered out.
- **ACTIVE**: the same 20 ms period + 3-consecutive-identical-reading debounce from v3 is kept, but implemented the same way as the existing `ACTIVE_LOG_PERIOD_MS` log output (a non-blocking comparison of `k_uptime_get_32()` values) instead of `k_msleep()`. Since ACTIVE mode's entire reason for existing is "the CPU stays ready at all times, so WFI never runs," adding even a single `k_msleep()` in this branch would itself reintroduce the idle gap ACTIVE is meant to eliminate.
- With only one thread left (`main()`), the `button_poll_task` vs. `main()` priority arrangement that v3 needed is also gone — there is nothing left that could be starved.

## Note

This change is independent of pinning down the gpio-keys driver bug (Issue 6). v4 still uses the same raw-GPIO-read approach from v3 (`gpio_pin_get_dt()`), so the diagnostic plan set out in Issue 6 (stopping after the first transition points to the I2C/hardware level; continuing to work points to the gpio-keys driver) still applies equally to v4. This lab had not yet completed hardware testing with either v3 or v4 at this point.

---

# Hardware Verification Result (2026-09-21) — v4 Final Confirmation, and a Documented Latency Characteristic

## Result: the gpio-keys Bypass (Raw GPIO Polling) Confirmed to Work Reliably on Hardware

After flashing v4 and pressing SW8 repeatedly many times, **ACTIVE↔IDLE toggling kept working correctly with no "stops after the first transition" symptom.** This finally confirms the diagnosis reached across Issues 4–7: the cause was not the I2C1/`gpio_exp0` hardware level but **a bug in this SDK's Zephyr `gpio-keys`/`input` subsystem driver's internal state machine** (the exact internal mechanism remains unidentified, but it is no longer pursued within this lab's scope), and it was fully bypassed via direct plain-GPIO polling.

## Known Characteristic (Not a Bug): Up to ~2 Second Delay on the IDLE → ACTIVE Transition

Because v4 samples the button only once, right after `k_msleep(IDLE_SLEEP_MS)` (2 seconds) in IDLE mode, there is a delay of **up to about 2 seconds, roughly 1 second on average**, between pressing SW8 and actually transitioning to ACTIVE mode, depending on when in that window the press occurs (the ACTIVE → IDLE direction responds essentially instantly, since it polls every 20 ms). This is a direct consequence of the trade-off intentionally chosen in Issue 7, and the final conclusion is as follows:

- **This is an intentionally retained characteristic.** Responding faster would require waking up more often even during IDLE, which would bring back the "IDLE current measurement contamination" problem fixed in Issue 7. In other words, **"low power" and "fast responsiveness" are inherently a trade-off in this architecture**, and since this lab's purpose is current measurement, measurement purity was chosen over responsiveness.
- There is, in theory, a way to reduce this latency: configuring a raw interrupt directly via `gpio_pin_interrupt_configure_dt()` + `gpio_add_callback()`, bypassing the gpio-keys/input subsystem (instant wake, no polling overhead). However, since the failure in Issue 4 was also on an interrupt-based path, there is a risk of running into the same unidentified class of driver bug again, and it would require additional debugging beyond this lab's learning objective (measuring the ACTIVE/IDLE current baseline). **This curriculum does not pursue it, and it is left only as a future experiment idea.**

## Conclusion

Lab 01 is finalized with this result. ACTIVE↔IDLE toggling, the ACTIVE/IDLE current measurements themselves, and the IDLE→ACTIVE latency characteristic documented above have all now been confirmed on hardware.

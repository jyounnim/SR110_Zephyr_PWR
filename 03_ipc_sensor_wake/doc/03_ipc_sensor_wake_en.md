# Lab 03 — M4 Accelerometer + mbox: "Live Wake" Without a Reset

## Learning Objectives

- Build an M4 -> M55 one-way notification channel with the standard Zephyr mbox API (`mbox_dt_spec`, `mbox_register_callback_dt()`, `mbox_set_enabled_dt()`, `mbox_send_dt()`).
- Read the onboard accelerometer (`mc3479`, on I2C1) from M4 with the standard Zephyr sensor API (`sensor_sample_fetch()`/`sensor_channel_get()`), and learn the pattern of calibrating a baseline (gravity/mounting offset) at boot.
- Learn a **send-on-change** design -- notifying only when the state actually flips, not on every sample -- to avoid notification spam.
- Get hands-on confirmation that mbox's rx callback runs in **ISR context**, and that it must never make a blocking call (including `k_msleep()` or `mbox_send_dt()`) -- it must hand off to a `k_msgq` and return immediately.
- After Lab 01 (WFI, woken by a timer, resumes exactly where it left off) and Lab 02 (watchdog, restarts from scratch on reset), confirm a **third kind of "wake"**: M55 never dies and never resets, staying alive the whole time and reacting only the moment another core's message arrives.

## Connection to Previous Labs

Lab 01 covered a "shallow sleep" -- M55 briefly sleeping on its own (WFI) and waking on a timer. Lab 02 covered a "deep sleep" -- M55 halting completely and restarting from scratch via reset. Lab 03 is a third pattern, different from both.

- M55 never resets and never arms its own timer. It blocks indefinitely on `k_msgq_get(..., K_FOREVER)`, waiting only for a message from M4.
- Even in this state, Zephyr's idle thread automatically enters WFI -- the same mechanism as Lab 01, but the difference is *what* wakes it: not "a timer expiring on a fixed schedule" but **a live message from another core**.
- So "waking without a reset" is the same as Lab 01, but the wake trigger is **another core's event** rather than its own timer -- a new category distinct from the previous two labs.

The mbox/sensor design patterns in this lab (ISR safety rule, baseline calibration, send-on-change) were carried over as-is from an already hardware-verified M4<->M55 IPC curriculum (a separate git repository, targeting the same board/SoC). **Hardware-confirmed (2026-09-21)**: this code ran correctly on this repository too, with no modifications needed -- the devicetree node names (`mbox_consumer`/`ipc0`/`mc3479`) and Kconfig symbols (`CONFIG_MC3419`/`CONFIG_MBOX`) all matched this SDK exactly, and shaking the board delivered M4->M55 events without any drops.

## Hardware Setup

- Keep both the M55 and M4 consoles open (230400bps 8N1, or the onboard bridge at 115200bps -- same as previous labs).
- The onboard accelerometer (`mc3479`) must be wired to M4's I2C1. No external wiring is needed.
- Boot with the board sitting flat and still, so baseline calibration captures the correct offset (moving the board during calibration folds that motion into the baseline -- see "Code Structure" below).

## Devicetree Overlays

The I2C1 bus-sharing issue from Lab 01 reappears here, but in the **opposite direction**. In Lab 01, M55 used I2C1 and M4 gave it up; in Lab 03, M4 needs I2C1 to read the accelerometer, so M55 gives it up instead.

`lab/boards/sr100_rdk_sr100_m55.overlay` (M55 -- disable I2C1 and all of its children):
```dts
&gpio_exp0 {
	status = "disabled";
};

&ov02c10 {
	status = "disabled";
};

&i2c1 {
	status = "disabled";
};

&ipc0 {
	shared-memory-size = <0x400>;
};
```

`lab/remote/boards/sr100_rdk_sr100_m4.overlay` (M4 -- explicitly enable only `mc3479`):
```dts
&mc3479 {
	status = "okay";
};

&ipc0 {
	shared-memory-size = <0x400>;
};
```

As confirmed in Lab 01's troubleshooting doc, **disabling only the parent bus (`i2c1`) is not enough** -- a child node that doesn't set its own `status` stays at the base devicetree default (`okay`) and keeps referencing the now-disabled parent, which fails to compile (a `__device_dts_ord_23 undeclared` -style error). That's why `gpio_exp0` and `ov02c10` are also disabled explicitly here.

`i2c1` itself is already `okay` by base devicetree default, so the M4 overlay doesn't need to turn it on separately (the mirror image of Lab 01's M55 overlay). `mc3479`, on the other hand, starts as `status = "disabled"` in the base devicetree and must be enabled explicitly.

`ipc0`'s `shared-memory-size` must be exactly the same value in both the M55 and M4 overlays.

**Hardware-confirmed (2026-09-21)**: the `gpio_exp0`, `ov02c10`, `i2c1`, `ipc0`, and `mc3479` node labels, and the `mbox_consumer` path, all built and worked with no changes -- the names carried over from the separate IPC curriculum repository matched this repository's devicetree exactly.

## Code Structure

### Shared header (`lab/include/ipc_common.h`, `lab/remote/include/ipc_common.h`)

Since only one message type ever flows, M4 -> M55, a single struct is enough without a separate type/command tag.

```c
enum motion_state {
	MOTION_STATE_STILL = 0,
	MOTION_STATE_ACTIVE = 1,
};

struct ipc_motion_event {
	uint32_t seq;          /* event sequence number, starts at 1 */
	uint8_t state;         /* enum motion_state: the state just entered */
	int32_t deviation_mg;  /* |dx|+|dy|+|dz| from the baseline, in
				 * milli-g, at the moment the state changed */
};
```

Following the established convention from the earlier IPC curriculum, this header is duplicated into both the M55 and M4 `include/` folders.

### M4 (`lab/remote/src/main.c`) -- accelerometer monitor

1. **Baseline calibration**: right after boot, average about 1 second of samples (`ACCEL_CALIB_SAMPLES = 10`, 100ms apart) to capture the gravity/mounting offset as a one-time snapshot. Every later sample is then compared against this baseline (not zero) to decide the state. Note that moving the board during calibration folds that motion into the baseline -- this is a fixed snapshot, not an adaptive filter.
2. **Sensor init order**: `mc3479` stays in a low-power standby state right after init, and silently returns all-zero readings (no error) until both the output data rate (ODR) and full-scale range are explicitly set via `sensor_attr_set()`. Since skipping either one produces the exact same symptom ("always zero"), the code keeps the two calls right next to each other to make this easy to spot.
3. **Send-on-change**: rather than sending on every sample, `mbox_send_dt()` is called only at the moment the state (`MOTION_STATE_STILL`/`MOTION_STATE_ACTIVE`) actually flips. A naive "send whenever over threshold" design would instead spam a notify on every single poll while the board sits near the threshold.
4. `mbox_send_dt()` is only ever called from `main()`'s own thread context (the polling loop). M4 never opens an rx callback in this lab (a one-way design where M55 never replies), so there is no way to accidentally call `mbox_send_dt()` from ISR context.

### M55 (`lab/src/main.c`) -- listener

1. Only the rx side is opened, via `mbox_register_callback_dt()` + `mbox_set_enabled_dt()`. Since this lab is a one-way "wake" channel with no M55 -> M4 reply, M55 never calls `mbox_send_dt()` at all.
2. **The rx callback runs in ISR context** -- an established rule, already hardware-confirmed, from this project's separate M4<->M55 IPC curriculum. So the callback only copies the message into the queue with `k_msgq_put(..., K_NO_WAIT)` and returns immediately; it does no real processing, including no logging.
3. The actual processing (logging here) happens in `main()`'s own while loop, in its own thread context, after pulling a message off the queue with `k_msgq_get(&evt_msgq, &evt, K_FOREVER)`.
4. Why block on `K_FOREVER`: in this lab, "no message" carries no meaning of its own (it just means nothing has moved yet). Unlike a heartbeat watchdog, where "nothing arrived within some window" is itself the signal to detect, there's no reason for a finite timeout here. With no other runnable thread in this state, Zephyr's idle thread automatically enters WFI.
5. **A point that's easy to conflate**: `STILL`/`ACTIVE` is the **motion state** M4's accelerometer sees, while WFI/idle is M55's **CPU power state** -- two different axes. It is not that "M55 goes idle only when the event is `STILL`" -- rather, whether the event was `ACTIVE` or `STILL`, once it's handled the loop **always** goes straight back to `k_msgq_get(K_FOREVER)` at the top, and WFI holds at that blocking point until the next event. Leaving the board still simply means no events arrive, which is why the blocked state looks like "idle" for a long stretch.

## prj.conf

`lab/prj.conf` (M55) -- a minimal configuration, since M55 only uses mbox rx in this lab:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_MBOX=y
```

`lab/remote/prj.conf` (M4) -- adds I2C1, the sensor subsystem, the `mc3479` driver, and mbox:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_I2C=y
CONFIG_SENSOR=y
CONFIG_MC3419=y

CONFIG_MBOX=y
```

**Hardware-confirmed (2026-09-21)**: both `CONFIG_MC3419` (the driver's Kconfig symbol; the devicetree node label is `mc3479`) and `CONFIG_MBOX` built successfully in this repository as-is.

## Build Instructions

Same approach as the previous labs (build M4 first, then package it into M55).

```bash
# 1) Build M4 first.
west build -p always -b sr100_rdk/sr100/m4 -d m4 03_ipc_sensor_wake/lab/remote

# 2) Build M55, packaging the M4 image alongside it.
west build -p always -b sr100_rdk/sr100/m55 -d m55 03_ipc_sensor_wake/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

## Running It and Checking the Results

**Hardware-confirmed (2026-09-21)**: this worked correctly on the very first build/flash, with no changes needed.

1. Flash with both the M4 and M55 consoles open. Leave the board sitting flat and still.
2. The M55 console goes quiet right after boot, waiting for events.
   ```
   === Lab 03 (M55): waiting for M4 motion-state notifications ===
   [M55] idle, waiting for events (CPU goes to WFI between them)...
   ```
3. Picking the board up, shaking it, and setting it back down repeatedly produces one event per state transition on the M55 console (an actual observed log):
   ```
   [M55] event seq=1: M4 is now ACTIVE (deviation=2533 milli-g)
   [M55] event seq=2: M4 is now STILL (deviation=1768 milli-g)
   [M55] event seq=3: M4 is now ACTIVE (deviation=2511 milli-g)
   [M55] event seq=4: M4 is now STILL (deviation=1056 milli-g)
   [M55] event seq=5: M4 is now ACTIVE (deviation=2084 milli-g)
   [M55] event seq=6: M4 is now STILL (deviation=969 milli-g)
   [M55] event seq=7: M4 is now ACTIVE (deviation=2285 milli-g)
   [M55] event seq=8: M4 is now STILL (deviation=958 milli-g)
   ```
   We confirmed that notifications don't arrive on every 100ms poll while shaking -- `seq` only increases once per state transition, regardless of how long the shaking lasts -- so the send-on-change design works as intended.
4. With the default `ACCEL_THRESHOLD_MILLI_G = 2000` left untouched, deviation while STILL stayed under the threshold (roughly 1000 milli-g or less) and deviation while ACTIVE cleared it comfortably (roughly high 2000s), with a clean separation and no false positives -- no retuning needed.
5. **A debug log added purely to check this**: to visually confirm that the loop always returns to `k_msgq_get(K_FOREVER)` right after handling an event -- regardless of whether it was `ACTIVE` or `STILL` -- and that WFI becomes possible again at exactly that point, a temporary `"[M55] back to waiting..."` log line was added and used to verify this. It was removed during code cleanup once verified (it is not in the final code). Confirming that the CPU actually returned to WFI (for example via a current measurement) would still require a separate multimeter measurement, as in Lab 01.

## Open Items (TBD)

- ~~This code had not been tested on hardware in this repository yet~~ -- **Confirmed (2026-09-21)**: the devicetree node names and Kconfig symbols all built and worked as-is, with no changes.
- ~~`ACCEL_THRESHOLD_MILLI_G = 2000` might need retuning for this board~~ -- **Confirmed**: the default value cleanly separates STILL/ACTIVE with no false positives.
- Exactly which physical range (+/-2g, +/-4g, etc.) `range.val1 = 0` in `sensor_attr_set(..., SENSOR_ATTR_FULL_SCALE, &range)` selects (the driver's "default/narrowest range") is still unconfirmed -- since it worked correctly in practice, we're not pursuing this further within this curriculum's scope.
- Whether the mbox callback truly runs in ISR context, and whether `mbox_send_dt()` can block on this board (an issue found in Lab 07 of the separate IPC curriculum), were not directly exercised by this test (the workload never generated the load that would have surfaced either issue) -- we're relying on having followed the established safe pattern by design.

## Next Lab Preview

Through Lab 03, we've now hands-on tested and hardware-confirmed all three distinct kinds of "wake" from Lab 00~02 (source review -> WFI -> watchdog reset) plus this one. Lab 04 is the most exploratory lab in this curriculum: an experiment to find, on real hardware, the relationship between `RESET_LOW_POWER_WAKE` and the board's physical Wake-up button (suspected to be `SW7`). Success isn't guaranteed, so we'll document honestly whatever we manage to find.

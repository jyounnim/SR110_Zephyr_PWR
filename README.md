# SR110 Zephyr Power Mode Lab Curriculum

A hands-on/seminar curriculum that hardware-verifies power management and wake mechanisms between the Cortex-M55 (main core) and Cortex-M4 (Always-On core) on Synaptics' **SR110 (Astra Machina, SR100 SoC)** RDK board, using Zephyr OS (`syna_zephyr_sdk-1.0.0`).

## Core questions this curriculum asks

- What is the official way for M4 (Always-On) to wake M55 (the core whose power state actually changes)?
- Once M55 wakes, does it resume execution right where it left off (WFI), or does it restart from scratch (reset)?
- Can the Active / Low-power / Ultra-low-power modes that SR110 appears to support at the hardware level actually be exercised from software with this SDK version?

The short answer to the third question, stated up front: **this SDK version has no such API** — see the curriculum wrap-up (Lab 05) and the "Overall findings" section below.

## Folder layout

Each lab is one `NN_topic/` folder, containing both code (`lab/`) and docs (`doc/`).

```
NN_topic/
  lab/
    src/main.c          # M55 code
    remote/src/main.c   # M4 code
    boards/*.overlay    # devicetree overlays (only labs that need one)
    CMakeLists.txt, prj.conf (M55 and M4 each)
  doc/
    NN_topic_kr.md                     # Korean lecture doc (source)
    NN_topic_en.md                     # English lecture doc (translated after Korean review)
    NN_topic_troubleshooting_kr.md     # (where present) detailed write-up of issues found during hardware debugging
```

Every lab builds the same way — M4 is built first, and its output is packaged into the M55 build.

```bash
west build -p always -b sr100_rdk/sr100/m4 -d m4 NN_topic/lab/remote
west build -p always -b sr100_rdk/sr100/m55 -d m55 NN_topic/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

## Lab list

| Lab | Title | Status | One-line summary |
|---|---|---|---|
| [Lab 00](00_sdk_and_schematic_review/doc/00_sdk_and_schematic_review_en.md) | SDK/schematic source review | Complete (EN available) | Confirms this SDK has no `CONFIG_PM`, and that the schematic's button/RTC/PIR share a `PMU_EN` path |
| [Lab 01](01_baseline_active_idle/doc/01_baseline_active_idle_en.md) | Active vs Idle (WFI) current baseline | Complete (EN available) | Toggles M55 ACTIVE/IDLE via an onboard button and measures the idle thread's automatic-WFI current savings |
| [Lab 02](02_watchdog_reset/doc/02_watchdog_reset_en.md) | M55's own watchdog reset | Complete (EN available) | Confirms `WDT_FLAG_RESET_SOC` does not self-complete and leaves the SoC stopped until a physical reset |
| [Lab 03](03_ipc_sensor_wake/doc/03_ipc_sensor_wake_en.md) | M4→M55 IPC (mbox) wake | Complete (EN available) | M4's accelerometer state change notifies M55 over mbox — the one path confirmed to work smoothly, with no reset |
| [Lab 04](04_low_power_wake/doc/04_low_power_wake_en.md) | `SW7`/`PMU_EN`/`RESET_LOW_POWER_WAKE` exploration | Complete (EN available) | `SW7` has no effect while the SoC is powered on — there's no API to enter the low-power mode it's meant to wake from |
| [Lab 05](05_reset_cause_design_guide/doc/05_reset_cause_design_guide_kr.md) | Synthesis: a reset-cause-based design guide | Complete (Korean only) | Combines a boot-time reset-cause dispatch with Lab 03's mbox pattern; includes a Lab 01-04 comparison table |

## Overall findings for this SDK/board combination

- **There is no intermediate deep-sleep state that preserves context.** Only two levels exist: WFI (shallow, resumes immediately) or reset (deep, full reboot).
- **The only officially confirmed, reliably working way for M4 to wake M55 is mbox (Lab 03).** The `RESET_LOW_POWER_WAKE`/`PMU_EN` path (Lab 04) is confirmed to exist at the hardware level but has no software entry point.
- **Watchdog-based reset (Lab 02) cannot be trusted as a safety mechanism.** Once triggered, it leaves the SoC stopped with no guaranteed automatic recovery.

## What we wished we could have covered

The original goal of this curriculum was to hands-on exercise the **Active / Low-power / Ultra-low-power modes** that SR110 appears to support at the hardware level. But as Labs 00-04 confirmed, this SDK version (`syna_zephyr_sdk-1.0.0`) has no software API to enter those modes at all (no `CONFIG_PM`). So in the end, what this curriculum could actually cover, at the Zephyr/software level, was only **toggling between Idle → Sleep (WFI) → back to Active** (Lab 01), plus two reset-based paths (watchdog reset, the PMU_EN family). In particular, even the **watchdog-based power reset we hoped could serve as a safety mechanism turned out not to self-complete** (Lab 02), so this curriculum did not reach its original goal of a genuine low-power-mode transition. Actually opening up this hardware-level low-power mode will likely require a future SDK release from Synaptics, or a direct inquiry to them.

## Notes

- Each lab's doc is written in Korean first and translated to English after review.
- Significant SDK/driver issues found during hardware debugging are documented separately in each lab's `*_troubleshooting_kr.md` / `*_troubleshooting_en.md` (Lab 01, Lab 02).
- The full design background, decision history, and hardware-verification log for this curriculum live in the Claude Project's master doc (`sr110-power-mode-curriculum-draft.md`).

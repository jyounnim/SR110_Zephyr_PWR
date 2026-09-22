# Lab 02 — M55 자체 Watchdog: Feed하지 않으면 정말 리셋되는가

## 학습 목표

- Zephyr 표준 Watchdog API(`wdt_install_timeout()`, `wdt_setup()`, `wdt_feed()`)로 M55 전용 하드웨어 watchdog(`wdog0`)을 실습으로 다룹니다.
- `WDT_FLAG_RESET_NONE`(콜백만, 실제 리셋 없음)과 `WDT_FLAG_RESET_SOC`(진짜 SoC 리셋) 두 모드의 차이를 실기로 직접 확인합니다.
- `wdt_feed()`를 의도적으로 멈췄을 때 정말 리셋이 발생하는지, 그리고 그 시점이 이론(설정한 timeout의 2배)과 맞는지 실측합니다.
- `hwinfo_get_reset_cause()`로 재부팅 원인이 `RESET_WATCHDOG`로 보고되는지 확인하고, 이 API를 매 부팅 초입에서 항상 체크하는 표준 패턴을 익힙니다.
- 전역 변수가 리셋 후 초기값으로 되돌아가는 것을 직접 확인해, 이 리셋이 "멈췄던 지점 재개(warm resume)"가 아니라 **처음부터 다시 시작하는 완전한 콜드 리부팅**임을 Lab 01(WFI, 컨텍스트 보존)과 대비해서 체감합니다.

## Lab 00/01과의 연결

Lab 00(SDK 소스 리뷰)에서 `wdog0`(`compatible = "syna,watchdog"`, `dts/arm/syna/astra_sr/sr100_m55.dtsi`)이 표준 Zephyr `wdt_driver_api`(`setup`/`disable`/`install_timeout`/`feed`)를 구현하고 있음을 확인했습니다. 이 리뷰에서 특히 중요한 두 가지 확정 사실이 있습니다.

1. **`WDT_FLAG_RESET_CPU_CORE`는 이 드라이버에서 명시적으로 거부됩니다(`-EINVAL`)** — "이 코어만 리셋"은 지원하지 않습니다. 지원되는 것은 `WDT_FLAG_RESET_NONE`(콜백만, 실제 리셋 없음) 또는 `WDT_FLAG_RESET_SOC`(진짜 SoC 리셋)뿐입니다. **실기 확인(2026-09-21)**: 이 제약은 이름 그대로였습니다 — `RESET_SOC`는 M55만이 아니라 **M4까지 포함한 SoC 전체를 멈추는 리셋**이었습니다(아래 "하드웨어 구성"과 트러블슈팅 문서 이슈 2 참고). `CPU_CORE`가 애초에 지원되지 않는 이유(M55만 골라 리셋하는 것 자체가 이 하드웨어에서 불가능해 보임)와 정확히 앞뒤가 맞습니다.
2. Lab 00의 소스 리뷰 당시에는 "카운터가 한 번 만료되면 콜백이 호출되고, feed하지 않으면 두 번째 만료 시점(timeout의 2배)에 실제 리셋 또는(`RESET_NONE`이면) 무한루프가 발생한다"고 추정했으나, **이 랩에서 `RESET_NONE`을 실기로 테스트한 결과 이 추정은 틀린 것으로 확인되었습니다** — 무한루프도, 리셋도 없이 콜백이 매 timeout 주기마다 계속 반복해서 호출됩니다(아래 "코드 구성"과 트러블슈팅 문서 이슈 1 참고). `RESET_SOC`가 실제로 리셋을 일으키는지는 이 문서의 "실행 및 결과 확인" 절에서 다루는 Step 2로 별도 확인이 필요합니다.

Lab 01은 "리셋 없이 이어서 실행되는 얕은 절전(WFI)"을 다뤘습니다. Lab 02는 그 반대편 — **"완전히 처음부터 다시 시작하는 깊은 절전(리셋)"**의 첫 사례입니다. 같은 "SoC가 다시 켜진다"는 결과라도, WFI는 레지스터/RAM을 그대로 보존한 채 멈췄던 지점부터 재개하는 반면, watchdog 리셋은 진짜 콜드 부팅(`main()`부터 재시작, 전역 변수 초기화)이라는 점이 이 랩의 핵심 대비 포인트입니다.

## 하드웨어 구성

- M55 콘솔만 있으면 충분합니다(230400bps, 8N1 또는 온보드 브릿지 115200bps — Lab 01과 동일).
- M4 콘솔도 함께 열어두는 것을 권장합니다. ~~M55가 자체 watchdog으로 리셋되는 동안 M4의 heartbeat가 끊기지 않는지 확인해, 이번 리셋이 M55 국소적임을 확인~~ — **실기 확인 결과(2026-09-21), 이 예상은 틀렸습니다.** `WDT_FLAG_RESET_SOC` 발동 시 M4의 heartbeat도 함께 멈춥니다. 즉 이 watchdog의 "SOC reset"은 이름 그대로 M55만이 아니라 **SoC 전체(M4 포함)를 멈추는 리셋**입니다. M4 콘솔을 같이 열어두는 이유는 이제 반대로 바뀌었습니다 — "M4가 계속 도는지"가 아니라 **"M4도 같이 멈추는지"를 확인하기 위해서**입니다. 상세는 트러블슈팅 문서 이슈 2를 참고하세요.
- 버튼/센서 등 별도 배선은 필요 없습니다.

## Devicetree 오버레이

`wdog0`은 base devicetree에서 `status = "disabled"`로 시작하므로, 이 랩이 이 프로젝트에서 처음으로 오버레이에서 `okay`로 켭니다.

`lab/boards/sr100_rdk_sr100_m55.overlay`:
```dts
&wdog0 {
	status = "okay";
};
```

M4/M55 공유 리소스(I2C1 등)를 전혀 쓰지 않으므로, Lab 01에서 다뤘던 버스 공유 문제는 이 랩에 해당하지 않습니다. M4 쪽 오버레이는 필요 없습니다.

## 코드 구성

### M55 (`lab/src/main.c`)

파일 최상단의 컴파일 스위치 하나로 두 가지 실습을 전환합니다.

```c
#define WDT_TEST_USE_RESET_SOC 0   /* 0 = WDT_FLAG_RESET_NONE, 1 = WDT_FLAG_RESET_SOC */
```

**공통 흐름**:
1. `main()` 진입 즉시 `hwinfo_get_reset_cause()`를 호출해 이번 부팅의 원인을 비트마스크로 출력하고(`RESET_PIN`/`RESET_WATCHDOG`/`RESET_SOFTWARE`/`RESET_POR`/`RESET_LOW_POWER_WAKE` 디코딩), `hwinfo_clear_reset_cause()`로 초기화합니다. 이는 이후 랩(Lab 05)에서도 재사용할 표준 부팅 패턴입니다.
2. `static uint32_t boot_local_counter = 12345;`를 선언하고 그 값을 그대로 출력합니다. 이 변수는 어떤 형태로도 리셋 사이에 값을 보존하지 않으므로(배터리 백업 RAM 등을 이 랩에서 추가하지 않았습니다), **매 부팅마다 항상 12345로 다시 초기화되어 출력되는 것 자체가 "이 리셋이 완전한 콜드 리부팅"이라는 증거**입니다.
3. `wdt_install_timeout()`으로 타임아웃(`WDT_TIMEOUT_MS = 2000`)과 콜백을 등록하면서, 위 스위치에 따라 `flags`를 `WDT_FLAG_RESET_NONE` 또는 `WDT_FLAG_RESET_SOC`로 설정합니다. `wdt_setup()`으로 감시를 시작합니다.
4. 1초 간격으로 5회 `wdt_feed()`를 호출해, feed가 실제로 타임아웃을 막아준다는 것을 먼저 확인시켜 줍니다.
5. 이후 **의도적으로 feed를 멈추고**, 그 시점의 `k_uptime_get_32()` 값을 출력한 뒤, 250ms 간격으로 "아직 살아있음" 하트비트를 무한 출력합니다.
6. 콜백(`wdt_callback`)은 최초 만료 시점에 한 번 호출되어 로그를 남깁니다(호출 횟수를 `callback_count`로 함께 출력). **실기 확인 결과 (2026-09-21)**: `RESET_NONE` 빌드에서는 이후 리셋도 hang도 없이, 이 콜백이 `WDT_TIMEOUT_MS` 주기로 계속 반복해서 호출되며 `main()`의 heartbeat 로그도 함께 무한히 계속됩니다 — 최초 설계 시 예상했던 "무한루프로 정지"는 틀린 추정이었습니다(트러블슈팅 문서 이슈 1 참고). `RESET_SOC` 빌드에서 실제로 리셋이 발생하는지는 아직 실기로 확인되지 않았습니다.

### M4 (`lab/remote/src/main.c`)

Lab 01과 동일한 1초 heartbeat입니다. 변경 없음 — 이 랩에서 M4의 역할은 "M55가 리셋되는 동안에도 멈추지 않는다"는 것을 보여주는 대조군입니다.

## prj.conf

`lab/prj.conf` (M55) — watchdog API와 reset-cause API를 위해 두 개가 추가되었습니다:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_WATCHDOG=y
CONFIG_HWINFO=y
```

`lab/remote/prj.conf` (M4) — Lab 01과 동일, 변경 없음.

## 빌드 방법

Lab 01과 동일한 방식입니다(M4를 먼저 빌드한 뒤 M55에 패키징).

```bash
# 1) M4를 먼저 빌드합니다.
west build -p always -b sr100_rdk/sr100/m4 -d m4 02_watchdog_reset/lab/remote

# 2) M55를 빌드하면서 M4 이미지를 함께 패키징합니다.
west build -p always -b sr100_rdk/sr100/m55 -d m55 02_watchdog_reset/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`WDT_TEST_USE_RESET_SOC`를 바꾼 뒤에는 소스만 수정한 것이므로 보통 재빌드만으로 충분하지만, 이상 동작 시 `-p always`를 쓰고 있는지 확인하세요(오버레이 변경 시 빌드 캐시 문제는 Lab 01에서 이미 겪은 바 있습니다).

## 실행 및 결과 확인

### Step 1 — `WDT_FLAG_RESET_NONE` (기본값, `WDT_TEST_USE_RESET_SOC 0`)

**실기 확인 결과 (2026-09-21)**: 아래는 최초 작성 당시의 예상(Lab 00 소스 리뷰 기반 — "콜백 1회 후 무한루프로 정지")이었으나, 실기 테스트 결과 **완전히 다른 결과**가 확인되었습니다. 콜백이 한 번만 찍히고 멈추는 것이 아니라, **`WDT_TIMEOUT_MS`(2000ms) 주기로 계속 반복해서 찍히며, `main()`의 heartbeat 로그도 전혀 멈추지 않고 무한히 계속됩니다 — 리셋도, hang도 없습니다.** 상세 원인과 해석은 트러블슈팅 문서(이슈 1)를 참고하세요. 아래 절차는 이 정정된 결과를 반영해 갱신했습니다.

1. 위 상태로 빌드/플래시하고 M55 콘솔을 관찰합니다.
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
   ... (약 2000ms 후)
   >> [WDT] callback fired on channel 0 (call #2) -- NOT feeding, watching what happens next
   ... (이 패턴이 계속 반복됩니다 -- 멈추지 않습니다)
   ```
2. 콜백 로그(`>> [WDT] callback fired ...`)가 마지막 feed로부터 약 2000ms(설정한 `WDT_TIMEOUT_MS`) 후에 처음 나타나는지 확인합니다.
3. **그 뒤로도 콜백이 약 2000ms 간격으로 계속 반복해서 찍히고, `[main] still running, ...`도 함께 계속 찍히면서 `callback_count`가 1, 2, 3, ...으로 계속 올라가는지 확인합니다.** 재부팅 배너는 나타나지 않습니다(리셋이 없으므로) — 하지만 시스템도 멈추지 않습니다(hang도 없으므로). 이는 최초 설계 의도(무한루프로 정지)와 다른, 실기로 확인된 결과입니다.
4. (선택) M4 콘솔을 같이 보고 있었다면, `[M4 heartbeat]`도 M55와 무관하게 계속 올라가는 것을 확인할 수 있습니다 — 다만 이번엔 M55 자체가 멈추지 않으므로 이 확인의 의미는 크지 않습니다.

### Step 2 — `WDT_FLAG_RESET_SOC` (`WDT_TEST_USE_RESET_SOC`를 1로 바꾼 뒤 재빌드/재플래시)

**실기 확인 결과 (2026-09-21) — 예상과 다른, 중요한 결과**: 아래 절차는 "콜백 후 약 2배 시점에 재부팅 배너가 다시 뜬다"는 사전 가설을 바탕으로 작성됐지만, 실기 테스트 결과는 달랐습니다.

- 5회 feed → 중단 → 콜백 로그(`>> [WDT] callback fired ...`)까지는 예상대로 나타났습니다.
- 그런데 콜백 이후 **`main()`의 `[main] still running, ...` 하트비트 로그 자체가 완전히 멈췄습니다** — 콘솔에 그 어떤 로그도, 재부팅 배너(`*** Booting Zephyr OS ***`)도 다시 나타나지 않았습니다.
- 이때 **M4 콘솔도 함께 멈춰 있었습니다** (`[M4 heartbeat]`도 더 이상 올라가지 않음).
- 이 멈춘 상태에서 **보드의 물리 리셋 버튼을 누르면 정상적으로 재부팅**되었습니다.

즉 `WDT_FLAG_RESET_SOC`는 이름 그대로 **M55만이 아니라 SoC 전체(M4 포함)를 멈추는 리셋**이며, 확인된 범위에서는 **그 리셋이 스스로 완료되지 못하고(재부팅 배너가 다시 뜨지 않음) 멈춰 있는 상태로 남습니다.** 외부의 물리 리셋(PIN 리셋)이 있어야만 실제로 다시 부팅됩니다. 다시 말해 이 watchdog은 "타임아웃 시 자동으로 스스로 복구되는 안전장치"로 동작하지 않고 있습니다 — 오히려 트리거되면 외부 개입 없이는 복구되지 않는 정지 상태를 만드는 것으로 보입니다. 상세 분석과 남은 의문점은 트러블슈팅 문서 이슈 2를 참고하세요.

**아래 절차는 참고용으로 남겨두되, 실제로는 4번(리셋 원인 확인)까지 도달하지 못했다는 점을 감안해서 진행해 주세요** — 3~6번은 "만약 자동으로 재부팅된다면"을 가정한 원래 계획이며, 현재까지는 물리 리셋을 눌러야만 그 지점에 도달합니다.

1. 같은 방식으로 5회 feed → 중단 → 콜백 로그까지는 Step 1과 동일하게 나타납니다.
2. 콜백 로그 이후 `[main] still running, ...`이 완전히 멈추는지, 그리고 (M4 콘솔을 같이 보고 있다면) `[M4 heartbeat]`도 같이 멈추는지 확인합니다.
   ```
   [main] STOPPING FEED NOW at uptime=5066 ms -- watching for timeout/reset
   ...
   >> [WDT] callback fired on channel 0 (call #1) -- NOT feeding, watching what happens next
   >> [WDT] RESET_SOC mode: a real SoC reset MAY follow around 2000 ms from here -- not yet confirmed on this SDK
   (여기서 M55, M4 콘솔 모두 완전히 멈춤 -- 이후 아무 로그도 없음)
   ```
3. 일정 시간(예: 30초~1분) 기다려도 재부팅 배너가 다시 뜨지 않는지 확인합니다(=자동 복구가 안 된다는 뜻).
4. 보드의 **물리 리셋 버튼**을 눌러 정상적으로 재부팅되는지 확인합니다.

**실기 확인 결과 (2026-09-21)**: 재부팅 후 `reset cause bitmask`는 `0x00000008 -> RESET_POR`이었습니다. `RESET_WATCHDOG`도 `RESET_PIN`도 아니었습니다. 이 결과가 watchdog과 관련이 있는지 확인하기 위해 이 랩과 무관한 정상 상태에서 같은 버튼을 눌러본 결과도 **똑같이 `RESET_POR`**이었습니다 — 즉 **이 보드의 물리 System Reset 버튼은 watchdog 여부와 상관없이 항상 `RESET_POR`을 보고**합니다. 결과적으로 `hwinfo_get_reset_cause()`로 "워치독의 SoC 리셋 요청이 실제로 하드웨어에 반영됐는지"를 판단하는 것은 이 버튼으로는 불가능하다는 것이 최종 결론입니다 — 상세는 트러블슈팅 문서 이슈 2 참고.
5. `boot_local_counter`가 물리 리셋 후 여전히 `12345`로 출력되는지는 확인했으나(콜드 리부팅 자체는 정상), 이것이 "watchdog이 걸어준 리셋"인지 "물리 버튼이 만들어낸 POR"인지는 위 4번과 같은 이유로 구분할 수 없습니다.

## 확인 필요 사항 (TBD)

1. ~~물리 리셋으로 복구된 후의 `reset cause bitmask`가 `RESET_WATCHDOG`인지 `RESET_PIN`인지~~ — **확인 완료(2026-09-21): 둘 다 아니고 `RESET_POR`(`0x00000008`)이며, 이 보드의 물리 리셋 버튼 고유의 특성(watchdog과 무관하게 항상 POR)으로 확인되어 이 진단 경로는 막다른 길로 결론지었습니다** (트러블슈팅 문서 이슈 2 참고).
2. ~~`RESET_SOC` 모드가 실제로 리셋을 발생시키는지~~ — **확인 완료(2026-09-21), 최종 결론**: 콜백 이후 M55와 M4가 모두 멈추는 것은 확인됐으나, **그 리셋이 스스로 완료되어 재부팅되는 것은 한 번도 관찰되지 않았고, 물리 리셋 버튼을 눌러야만 복구됩니다.** 그 리셋 요청이 하드웨어적으로 실제 실행됐는지 여부는 이 보드의 물리 리셋 버튼이 항상 `RESET_POR`을 보고하는 특성 때문에 `reset cause`로는 확인이 불가능한 것으로 결론지었습니다 — 트러블슈팅 문서 이슈 2 참고.
3. `WDT_TIMEOUT_MS`를 더 짧게(예: 100ms) 또는 더 길게 설정했을 때도 이번에 확인된 "매 timeout마다 자동 재무장"(`RESET_NONE`) 및 "리셋이 걸리지만 자동 복구는 안 됨"(`RESET_SOC`) 동작이 동일하게 유지되는지, 그리고 이 SDK/드라이버가 지원하는 timeout의 최소/최대 범위(`WDT_SYNA_DEFAULT_TIMEOUT_MAX` 등)는 확인되지 않았습니다.
4. ~~`RESET_NONE` 모드에서 드라이버가 "무한루프"에 빠진다~~ — **확인 완료(2026-09-21), 사전 예상과 다름**: 무한루프에 빠지지 않고, 대신 콜백이 매 timeout 주기마다 계속 재발생하며 시스템은 정상 동작을 유지합니다. 정확한 내부 메커니즘(드라이버가 콜백 실행 후 항상 카운터를 재적재하는지, 인터럽트 클리어 과정에서 자동으로 재무장되는지 등)은 소스 재확인 없이는 특정하지 못했습니다 — 트러블슈팅 문서 이슈 1 참고.
5. ~~`RESET_SOC`가 M55만의 국소적 리셋인지~~ — **확인 완료(2026-09-21), 사전 가정과 다름**: M4까지 함께 멈추는 SoC 전체 리셋으로 확인되었습니다 — 트러블슈팅 문서 이슈 2 참고. 이 사실은 Lab 00/01 문서에서 "M4는 Always-On이라 M55의 전원 상태 변화와 무관하다"고 설명해 온 것과 정면으로 배치되므로, 다른 랩(특히 향후 IPC/Low-Power-Wake 랩)에도 영향을 줄 수 있는 중요한 정정 사항입니다.

## 다음 랩 예고 (Lab 03)

Lab 03은 기존 M4↔M55 IPC 커리큘럼의 mbox 패턴을 재사용해, M4가 센서 임계값을 넘었을 때만 M55에 notify를 보내는 구조를 다룹니다. Lab 01(WFI, 리셋 없음)·Lab 02(watchdog, 리셋 있음)에 이어, 이번엔 "리셋도 없고 WFI도 아닌, 살아있는 채로 다른 코어가 깨우는" 세 번째 유형의 wake를 다루게 됩니다. 이 랩에서 M4의 워크로드도 Lab 01/02의 고정 heartbeat에서 실제 센서 폴링으로 처음 교체됩니다.

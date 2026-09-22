# Lab 01 — Baseline: M55 Active vs Idle(WFI) 유휴전류 측정

## 학습 목표

- Zephyr 커널이 `CONFIG_PM` 설정과 무관하게 항상 제공하는 idle thread의 자동 WFI(Wait For Interrupt) 진입 동작을 이해합니다.
- 애플리케이션 스레드가 "항상 실행 가능한 상태(ready)"를 유지하는 것과 "블로킹 호출로 대기하는 것"이 CPU의 실제 전력 소모에 어떤 차이를 만드는지 직접 측정으로 확인합니다.
- M55를 온보드 버튼으로 ACTIVE/IDLE 두 모드 사이에서 실시간으로 전환하며, 재플래시 없이 하나의 펌웨어로 두 상태의 전류를 비교하는 방법을 익힙니다.
- 이 측정값을 이후 랩(watchdog 리셋, IPC wake, PMU_EN 기반 wake)의 전류·지연시간 비교 기준선(baseline)으로 삼습니다.

## Lab 00과의 연결

Lab 00(SDK 및 회로도 소스 리뷰)에서 확인한 핵심 사실은 다음과 같습니다.

- 이 SDK(`syna_zephyr_sdk-1.0.0`)는 Zephyr의 표준 Power Management 서브시스템(`CONFIG_PM`, `enum pm_state`)을 전혀 구현하지 않습니다.
- 따라서 이 SDK에서 애플리케이션이 실제로 쓸 수 있는 유일한 저전력 수단은, Zephyr 커널이 기본으로 제공하는 idle thread의 자동 WFI 진입뿐입니다. 이는 별도의 설정 없이 "실행 가능한 스레드가 하나도 없을 때" 항상 자동으로 동작합니다.

이 랩은 이 사실을 실측으로 확인하는 첫 번째 코드 랩입니다. 새로운 저전력 API를 추가하는 것이 아니라, **이미 존재하는 idle thread의 동작을 켜고 끄듯 관찰**하는 것이 목적입니다.

## 핵심 개념: Zephyr Idle Thread와 WFI

Zephyr 스케줄러는 실행 가능한(ready) 애플리케이션 스레드가 하나도 없는 순간, 자동으로 최저 우선순위의 idle thread를 실행합니다. ARM Cortex-M 아키텍처에서 이 idle thread는 `k_cpu_idle()`(내부적으로 CMSIS `__WFI()` 명령)을 호출하며, 이는 다음 인터럽트가 발생할 때까지 CPU 클럭을 게이팅해 대기하는 명령입니다. 레지스터와 RAM 내용은 그대로 보존되고, 인터럽트가 발생하면 WFI 다음 명령부터 즉시 재개됩니다 — Lab 00에서 정리한 것처럼 "컨텍스트를 잃지 않는, 가장 얕은 절전"입니다.

이 랩에서 두 모드의 차이는 다음과 같습니다.

| 모드 | 스레드 동작 | idle thread 실행 여부 | 예상 전류 |
|---|---|---|---|
| **ACTIVE** | 블로킹 호출 없이 `k_yield()`만 반복 — 스레드가 항상 ready 상태를 유지 | 실행되지 않음(WFI 없음) | 상대적으로 높음 |
| **IDLE** | `k_msleep(2000)`으로 2초씩 대기 — 그 사이에는 ready 스레드가 없음 | 대기 구간 동안 계속 실행됨(WFI 반복) | 상대적으로 낮음 |

`k_yield()`는 같은 우선순위의 다른 ready 스레드가 있으면 그쪽에 CPU를 양보하고, 없으면 호출한 스레드가 곧바로 계속 실행됩니다 — **idle thread에게 양보하는 함수가 아닙니다.** 그래서 ACTIVE 모드에서는 idle thread가 스케줄될 기회 자체가 없습니다.

## 회로/측정 준비물

- SR110 RDK 보드, 멀티미터(전류 측정 가능한 것, 가능하면 mA/µA 단위까지 표시되는 제품 권장)
- 이 랩은 LED·센서를 전혀 사용하지 않습니다 — 측정값이 CPU의 Active/Idle 상태만을 반영하도록 의도적으로 배제했습니다. 다만 아래 설계에 따라 온보드 버튼(SW8)을 읽기 위한 I2C1 GPIO 익스팬더는 M55에서 활성화합니다(전류에 미치는 영향은 미미할 것으로 예상되나 TBD).
- **TBD**: 보드에서 CPU/SoC 전류만 분리해서 측정할 수 있는 전용 측정 포인트(예: 전원 트리 상의 션트 저항이나 점퍼)가 있는지는 이번 리뷰로 확정하지 못했습니다. 별도 측정 포인트가 없다면, 보드 전체 입력 전원 라인(예: 배터리/USB 공급단)에 멀티미터를 직렬로 연결해 총 전류를 측정하고, M4의 heartbeat 워크로드가 고정되어 있다는 점(아래 "설계 노트" 참고)을 근거로 ACTIVE↔IDLE 전환 시의 **차이값**을 M55에 기인한 것으로 해석하시면 됩니다.

## 설계 노트 — 왜 M4는 고정된 heartbeat만 도는가

이 SoC는 M4가 Always-On이고 M55만 전원 상태가 바뀝니다. 이 랩의 목적은 "M55의 Active/Idle 전류 차이"를 측정하는 것이므로, 두 측정(ACTIVE vs IDLE) 사이에서 **M4의 소비 전류는 반드시 동일하게 고정**되어야 M55에 의한 차이만 깨끗하게 비교할 수 있습니다. 그래서 M4는 1초 주기로 heartbeat 로그만 출력하는 단순 워크로드로 고정했습니다. 실제 센서 기반 Always-On 감시(예: 온도 센서 폴링)는 아직 이 랩에 연결하지 않았습니다 — M4↔M55 IPC를 다루는 이후 랩(Lab 03)에서 실제 워크로드로 교체합니다.

## 하드웨어 구성

- **M55 콘솔**: 보드의 USB-C 커넥터(온보드 브릿지 경유, 115200bps) 또는 외부 UART-USB 컨버터로 연결하는 헤더(230400bps, 8N1) 중 편한 쪽으로 접속하시면 됩니다. 두 경로 모두 물리적으로 같은 M55 UART1 신호선입니다. 이 콘솔은 출력 전용(로그 확인용)입니다.
- **M4 콘솔**: 별도의 USB-UART 어댑터로 연결합니다. 정확한 커넥터 지정번호는 이 리뷰에서 아직 확인하지 못했습니다(**TBD**) — 이전 M4↔M55 IPC 랩에서 사용하신 것과 동일한 연결 방법을 그대로 쓰시면 됩니다.
- **모드 전환**: 보드의 온보드 푸시버튼 **SW8("USER_BUTTON")**을 누릅니다. 별도 배선은 필요 없습니다. 이 랩은 콘솔 UART의 RX 경로를 전혀 쓰지 않습니다.

## Devicetree 오버레이

이 랩은 온보드 버튼 SW8을 읽기 위해 M55에서 I2C1(및 그 뒤에 물린 GPIO 익스팬더 `gpio_exp0`)을 사용합니다. I2C1은 물리적으로 M4/M55가 공유하는 하나의 버스이므로(별도 진행한 M4↔M55 IPC 랩 시리즈에서 실기로 확인된 사항), 두 코어가 동시에 이 버스를 마스터로 초기화하려 하면 충돌합니다. 이 랩에서 M4는 I2C1을 쓰지 않으므로, **M55만 I2C1을 활성화하고 M4는 비활성화**합니다.

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

`ov02c10`(카메라 센서) 노드는 M55 쪽에서만 비활성화합니다 — M4 devicetree에도 동일한 노드가 존재하는지는 확인되지 않았고(**TBD**), 존재하지 않는 노드를 오버레이에서 참조하면 devicetree 컴파일 에러가 나므로 M4 쪽에는 포함하지 않았습니다.

## 코드 구성

### M55 (`lab/src/main.c`)

이 랩은 Zephyr의 `gpio-keys`/입력 서브시스템을 쓰지 않습니다 — SW8("USER_BUTTON")을 `user_button` devicetree 노드의 `gpios` 프로퍼티로 직접 가져와(`GPIO_DT_SPEC_GET()`), plain GPIO API(`gpio_pin_get_dt()`)로 읽습니다. (이 SDK의 `gpio-keys` 드라이버에는 실제로 버그가 있어 우회한 것입니다 — 상세는 `01_baseline_active_idle_troubleshooting_kr.md` 참고.)

버튼 폴링은 별도 스레드가 아니라 **`main()`의 기존 루프 안에 모드별로 다르게 통합**되어 있습니다. 전용 폴링 스레드를 따로 두면 IDLE 모드에서도 스레드가 계속 깨어나 CPU가 진짜 WFI로 잠들지 못하고, 그 결과 이 랩이 측정하려는 IDLE 전류 자체가 오염되기 때문입니다.

- **IDLE 모드**: 이미 상태 로그를 위해 실행하는 `k_msleep(2000)` 직후, 그 자리에서 버튼을 딱 한 번만 읽습니다. CPU가 그 시점에 어차피 깨어날 예정이었으므로 이 폴링은 IDLE의 전류 프로파일에 추가 wake 이벤트를 전혀 만들지 않습니다. 2초 간격 자체가 기계식 버튼 채터링(수 ms)보다 훨씬 길기 때문에 별도 디바운스 로직 없이 단일 샘플만으로 충분합니다.
- **ACTIVE 모드**: 20ms 주기로 3회 연속 동일 값이 나와야 상태 변화로 인정하는 sample-and-debounce 로직을 쓰되, 그 주기는 `k_msleep()`이 아니라 기존 `ACTIVE_LOG_PERIOD_MS` 로그와 동일한 방식(`k_uptime_get_32()` 값을 논블로킹으로 비교)으로 구현되어 있습니다. ACTIVE 모드의 존재 이유가 "CPU가 항상 ready 상태라 WFI가 절대 안 돈다"이므로, 이 분기 안에서 `k_msleep()`을 호출하면 그 자체로 다시 유휴 구간을 만들어버리기 때문입니다.
- 누름 엣지(0→1로 안정화되는 순간)에서만 모드를 토글합니다 — 누름+뗌 한 번을 토글 한 번으로 처리하기 위함입니다.
- 스레드가 `main()` 하나만 남아 있으므로, 우선순위 문제로 굶주릴 대상이 없습니다.

**실기 확인 완료(2026-09-21)**: SW8을 여러 차례 반복해서 눌러도 ACTIVE↔IDLE 토글이 계속 정상 동작함을 확인했습니다.

**알려진 특성(버그 아님)**: IDLE 모드는 버튼을 2초 주기로 한 번만 샘플링하므로, SW8을 눌러 IDLE→ACTIVE로 전환되기까지 최대 약 2초(평균 약 1초)의 지연이 있습니다. ACTIVE→IDLE 방향은 20ms 폴링이라 사실상 즉시 반응합니다. 이 비대칭은 의도된 트레이드오프입니다 — 더 빠르게 반응하려면 IDLE 중에도 더 자주 깨어나야 하는데, 그러면 이 랩이 측정하려는 IDLE 전류 자체가 오염됩니다. **"낮은 전력"과 "빠른 반응성"이 서로 트레이드오프 관계라는 것을 직접 체감할 수 있는 지점**이므로, 이 랩에서는 개선하지 않고 그대로 둡니다(더 빠른 반응이 필요하면 raw GPIO 인터럽트를 직접 구성하는 방법이 있으나, 이번 커리큘럼에서는 다루지 않습니다).

### M4 (`lab/remote/src/main.c`)

1초 주기로 `[M4 heartbeat] tick=N, uptime=... ms`를 출력하는 것이 전부입니다. M55의 모드와 무관하게 항상 동일하게 동작해, M55의 Active/Idle 전류 차이만 순수하게 비교할 수 있도록 합니다.

## prj.conf

`lab/prj.conf` (M55) — 콘솔 출력 설정에 더해, SW8을 plain GPIO로 읽기 위한 `CONFIG_GPIO`/`CONFIG_I2C`(I2C 익스팬더 경유)만 있으면 됩니다. Zephyr 입력 서브시스템(`gpio-keys`)을 쓰지 않으므로 `CONFIG_INPUT`/`CONFIG_INPUT_GPIO_KEYS`는 필요 없습니다:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_GPIO=y
CONFIG_I2C=y
```

`lab/remote/prj.conf` (M4) — 콘솔 출력에 필요한 최소 설정만 사용합니다:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y
```

## 빌드 방법

이 SoC는 M4를 먼저 빌드한 뒤, 그 결과물(M4 ELF)을 M55 빌드에 포함시켜 하나의 flash 이미지로 통합하는 방식입니다(별도 진행한 M4↔M55 IPC 랩 시리즈와 동일한 방식). 워크스페이스 루트에서 다음과 같이 진행합니다.

```bash
# 1) M4를 먼저 빌드합니다.
west build -p always -b sr100_rdk/sr100/m4 -d m4 01_baseline_active_idle/lab/remote

# 2) M55를 빌드하면서, 방금 만든 M4 이미지를 함께 패키징합니다.
#    M4_BUILD는 M55의 빌드 디렉터리(-d로 지정한 경로) 기준 상대경로입니다.
#    아래 예시는 두 빌드 디렉터리(m4/, m55/)가 같은 위치에 나란히 있는 경우입니다.
west build -p always -b sr100_rdk/sr100/m55 -d m55 01_baseline_active_idle/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

**참고**: 오버레이/devicetree를 변경한 뒤에는 `-p always`를 쓰고 있는지 확인하세요. 기존 빌드 디렉터리를 재사용하면 변경이 반영되지 않는 빌드 캐시 문제가 있을 수 있습니다.

플래싱은 OpenOCD 기반 스크립트(`srsdk_tools/openocd_flash.py`)로 진행합니다. 정확한 인자값(툴체인/설정 파일 경로)은 로컬 환경 설정에 따라 다르므로, 기존에 사용하시던 방식을 그대로 적용하시면 됩니다.

## 실행 및 결과 확인

1. M55와 M4 양쪽 콘솔을 각각 시리얼 터미널로 열어둡니다.
2. 플래시 후 재부팅하면, M55 콘솔에 다음과 같은 배너와 함께 IDLE 모드로 부팅됩니다.
   ```
   === Lab 01: M55 Active vs Idle (WFI) Baseline ===
   Press the onboard SW8 (USER_BUTTON) to toggle ACTIVE/IDLE mode
   Boot mode: IDLE
   [IDLE] uptime=12 ms -- sleeping 2000 ms (WFI expected)
   [IDLE] uptime=2013 ms -- sleeping 2000 ms (WFI expected)
   ...
   ```
   M4 콘솔에는 독립적으로 1초마다 `[M4 heartbeat] tick=N, uptime=... ms`가 출력됩니다.
3. 이 상태(IDLE)에서 멀티미터로 전류를 측정하고 값을 기록합니다.
4. 온보드 **SW8** 버튼을 한 번 눌러 ACTIVE 모드로 전환합니다. **IDLE→ACTIVE 전환은 설계상 최대 약 2초(평균 약 1초) 지연이 있으니, 버튼을 누른 즉시 반응이 없어도 정상입니다** — 잠시 기다려 보세요(위 "코드 구성" 절의 "알려진 특성" 참고).
   ```
   >> [SW8] ACTIVE mode: CPU stays ready, WFI never runs
   [ACTIVE] uptime=4021 ms -- CPU never idles
   [ACTIVE] uptime=6021 ms -- CPU never idles
   ...
   ```
5. 몇 초간 안정화를 기다린 뒤 같은 방식으로 전류를 측정하고 기록합니다.
6. SW8을 한 번 더 눌러 다시 IDLE로 돌아가 값이 원래대로 낮아지는지 재확인합니다.
7. ACTIVE 측정값에서 IDLE 측정값을 뺀 차이가, 이 SDK에서 순수 idle thread의 자동 WFI만으로 절감되는 M55의 baseline 전류입니다. 이 값을 기록해 두면, 이후 Lab 02(watchdog)·Lab 04(PMU_EN wake)에서 측정할 전류/지연시간과 비교할 기준선이 됩니다.

## 확인 필요 사항 (TBD)

1. 보드에 CPU/SoC 전류만 분리 측정할 수 있는 전용 측정 포인트가 있는지
2. M4 콘솔의 정확한 물리 커넥터 지정번호
3. M4 devicetree에 `ov02c10` 노드가 실제로 존재하는지(존재하지 않는 것으로 가정하고 M4 오버레이에서 제외함)
4. `k_yield()` 기반 ACTIVE 루프가 이 보드의 실제 클럭/전력 관리 하드웨어에서 "CPU가 100% 활성 상태를 유지한다"는 가정과 정확히 일치하는지는 실측으로 최종 확인 필요(이론적으로는 idle thread가 스케줄되지 않는 것으로 충분하다고 판단됩니다)

구현 과정에서 실기로 확인된 SDK 이슈(콘솔 RX 오염, `gpio-keys` 드라이버 버그 등)와 그 진단 과정은 `01_baseline_active_idle_troubleshooting_kr.md`에 별도로 정리되어 있습니다.

## 다음 랩 예고 (Lab 02)

Lab 02는 M55 전용 watchdog(`wdog0`)을 보드 오버레이에서 활성화해, `wdt_feed()`를 멈췄을 때 실제로 SoC 레벨 리셋이 발생하는지, 그리고 그 리셋까지 걸리는 시간이 이론(timeout의 2배)과 일치하는지를 실측으로 검증합니다. 이 랩(Lab 01)이 보여준 "리셋 없이 이어서 실행되는 얕은 절전(WFI)"과 대비되는, "완전히 처음부터 다시 시작하는 깊은 절전(리셋)"의 첫 사례입니다.

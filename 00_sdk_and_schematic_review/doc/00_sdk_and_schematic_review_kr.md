# Lab 00 — SR110 Power Mode: SDK 및 회로도(Schematic) 소스 리뷰

## 학습 목표

- Zephyr가 정의하는 표준 System Power State(`enum pm_state`)의 개념을 이해합니다.
- `syna_zephyr_sdk-1.0.0` 소스 코드를 직접 확인해, SR110(SR100 SoC) 보드에서 이 표준 Power State 중 실제로 무엇이 구현되어 있는지 확정합니다.
- SR110 RDK 회로도(SC950-C01116-01 RevE)를 통해, 소프트웨어(SDK) 레벨에서는 보이지 않는 하드웨어 레벨의 전원 시퀀싱/wake 트리거 구조를 파악합니다.
- M4(Always-On 코어)가 M55(전원 상태가 실제로 바뀌는 코어)를 깨우는 여러 경로를 정리하고, 각 경로가 "리셋을 동반하는 wake"인지 "리셋 없이 이어서 실행되는 wake"인지 구분합니다.
- 이후 Lab에서 실기로 검증해야 할 항목(TBD)을 명확히 식별합니다.

이 랩은 코드가 없습니다. SDK 소스 코드와 보드 회로도를 읽고 분석한 결과를 정리하는 조사(research) 랩이며, Lab 01부터 실제 코드 실습이 시작됩니다.

## 왜 코드보다 소스/회로도 리뷰부터 시작하는가

SR110의 두 코어(M55, M4) 중 **M4는 Always-On이고, 전원 상태가 실제로 바뀌는 것은 M55뿐**입니다. 이 커리큘럼의 핵심 질문은 "M4가 M55를 어떻게 깨우는가"와 "M55가 깨어난 뒤 멈췄던 지점에서 이어서 실행되는가, 아니면 처음부터 다시 부팅되는가"입니다.

이 질문에 답하려면 먼저 이 SDK 버전이 Zephyr의 표준 Power Management 서브시스템(`CONFIG_PM`)을 실제로 구현하고 있는지부터 확인해야 합니다. 구현 여부에 따라 이후 랩에서 다룰 수 있는 범위 자체가 완전히 달라지기 때문입니다. 또한 SDK 소스만으로는 보이지 않는 보드 레벨의 물리적 wake 트리거(버튼, RTC, PIR 센서 등)가 있는지도 회로도로 함께 확인해야, 소프트웨어로 구현된 경로와 하드웨어로만 존재하는 경로를 혼동하지 않을 수 있습니다.

## 1. 조사 대상

- **SDK**: `syna_zephyr_sdk-1.0.0` (사용자 제공, 압축 해제 후 전수 검토)
- **회로도**: `SC950-C01116-01 RevE` (SR110 RDK 보드, 13페이지 전체)

## 2. Zephyr 표준 System Power State (일반론)

Zephyr는 `enum pm_state`로 7단계의 System Power State를 정의합니다. 아래 표는 Zephyr 공식 문서 기준의 일반적인 정의이며, 3장에서 이 중 이 SDK에 실제로 구현된 것이 무엇인지 확인합니다.

| 상태 | 정의 | Wake 후 동작(일반론) |
|---|---|---|
| `PM_STATE_ACTIVE` | 완전히 켜져 정상 동작 중 | — |
| `PM_STATE_RUNTIME_IDLE` | 모든 코어가 가장 얕은 idle(WFI)로 대기 | 인터럽트 발생 즉시, 멈췄던 지점부터 재개 |
| `PM_STATE_SUSPEND_TO_IDLE` | 시스템 전체가 정상 suspend 절차를 거쳐 모든 코어가 idle | RUNTIME_IDLE과 유사, 즉시 재개 |
| `PM_STATE_STANDBY` | 주변장치 저전력 진입 + boot CPU가 아닌 나머지 코어는 전원 차단 | 전원이 차단됐던 코어는 재기동(re-boot) |
| `PM_STATE_SUSPEND_TO_RAM` | 메모리는 self-refresh로 보존, 나머지는 최대한 전원 차단 | RAM 내용이 보존된 채로 "warm resume" |
| `PM_STATE_SUSPEND_TO_DISK` | 메모리를 포함한 대부분의 전원을 차단 | 저장 매체에서 재적재 — 사실상 재부팅 |
| `PM_STATE_SOFT_OFF` | 최소 전력 상태, 복귀에 큰 지연시간 | `main()`부터 다시 시작하는 콜드 부팅과 사실상 동일 |

이 표에서 주목할 부분은 `RUNTIME_IDLE`/`SUSPEND_TO_IDLE`(리셋 없이 이어서 실행)과 `SOFT_OFF`(사실상 재부팅) 사이에, RAM 내용을 보존하면서도 깊이 자는 중간 단계(`SUSPEND_TO_RAM`, `STANDBY`)가 존재한다는 점입니다. 3장에서 이 SDK가 이 중간 단계를 지원하는지 확인합니다.

## 3. `syna_zephyr_sdk-1.0.0` 소스 리뷰 결과 (확정 사실)

아래 항목은 모두 SDK 소스 코드를 직접 열어 확인한 사실이며, 추측이 아닙니다. 각 항목에 근거 파일 경로를 함께 표기합니다.

### 3.1 `CONFIG_PM` 자체가 이 SDK에 구현되어 있지 않습니다

SDK 전체 트리에서 `pm_state_set`, `PM_STATE_*`, `power-states`, `zephyr,power-state` 문자열을 전수 검색(grep)했으나 단 한 곳에서도 발견되지 않았습니다. PM 서브시스템 관련 샘플이나 devicetree `power-states` 노드도 존재하지 않습니다.

**결론**: 2장의 7단계 System Power State는 이 SDK 버전에서 **하나도 구현되어 있지 않습니다.** 애플리케이션이 사용할 수 있는 저전력 수단은 Zephyr 커널이 기본으로 제공하는 idle thread의 자동 WFI(Wait For Interrupt) 진입뿐이며, 이는 `CONFIG_PM` 설정과 무관하게 항상 동작합니다.

### 3.2 M55 전용 Watchdog이 실재하며, 표준 Zephyr WDT API로 노출됩니다

- 노드: `wdog0: watchdog@50350038`, `compatible = "syna,watchdog"` — `dts/arm/syna/astra_sr/sr100/sr100_m55.dtsi`. 기본 `status = "disabled"`로, 보드 오버레이에서 명시적으로 켜야 합니다.
- M4는 별도의 워치독(`lp_wdt: watchdog@b5003000`, `compatible = "arm,apb-wdt"` — `sr100_m4.dtsi`)을 가지며, M4/M55는 서로 다른 워치독 IP이지만 둘 다 같은 공유 레지스터 `AON_POR_RST`(주소 `0x50350038`, `soc/syna/astra_sr/sr100/soc.c`)에 리셋 원인 비트를 기록합니다: M4 워치독 = bit 8(`AON_POR_RST_M4_WATCHDOG_BIT`), M55 워치독 = bit 9(`AON_POR_RST_M55_WATCHDOG_BIT`).
- 드라이버(`drivers/watchdog/wdt_syna.c`)는 표준 `wdt_driver_api`(`setup`/`disable`/`install_timeout`/`feed`)를 구현합니다.
- **`WDT_FLAG_RESET_CPU_CORE`는 명시적으로 거부됩니다(`-EINVAL`을 반환)** — "이 코어 하나만 리셋"하는 기능은 지원하지 않습니다. 지원되는 것은 `WDT_FLAG_RESET_NONE`(콜백만 호출, feed하지 않으면 이후 무한루프에 진입할 뿐 실제 리셋은 없음)과 `WDT_FLAG_RESET_SOC`(SoC 전체 리셋) 두 가지뿐입니다.
- 드라이버 주석에 따르면, 카운터가 한 번 만료되면 먼저 등록된 콜백이 호출되고(있다면), 그 안에서 feed하지 않으면 **두 번째 만료 시점(설정한 timeout의 2배가 지난 시점)**에 실제 리셋이 발생합니다.

**결론**: M55 watchdog timeout은 진짜 SoC 레벨 하드웨어 리셋입니다. "멈췄던 지점에서 이어서 실행"이 아니라 **`main()`부터 처음부터 다시 시작하는 완전한 콜드 리부팅**입니다. 사용자께서 처음에 가정하신 "ultra low power로 갔다가 wake up되면 rebooting" 시나리오와 정확히 일치하는 경로입니다.

### 3.3 `hwinfo_get_reset_cause()`가 5가지 리셋 원인을 표준 Zephyr API로 구분합니다

근거: `drivers/hwinfo/hwinfo_syna_sr100.c`.

| 레지스터 비트 마스크 | Zephyr 표준 플래그 | 의미 |
|---|---|---|
| `SR100_RST_PIN_MASK` (bit 12) | `RESET_PIN` | 물리 리셋 핀/버튼 |
| `SR100_RST_POR_MASK` (bit 13) | `RESET_POR` | 아날로그 PMU의 파워온 리셋 |
| `SR100_RST_WDOG_MASK` (bit 8~9) | `RESET_WATCHDOG` | M4 또는 M55 워치독 (3.2절) |
| `SR100_RST_SW_MASK` (bit 10~11) | `RESET_SOFTWARE` | 소프트웨어에 의한 리셋 |
| `SR100_RST_AON_LP_MASK` (bit 0~7) | `RESET_LOW_POWER_WAKE` | AON(Always-On) 또는 M4/LPPROC 도메인이 유발하는 저전력 wake |

`hwinfo_get_reset_cause()`/`hwinfo_clear_reset_cause()`(clear 시 `0x3fff` 기록)가 이 API를 구현합니다.

**`RESET_LOW_POWER_WAKE`의 존재 자체가 핵심 증거입니다.** 이 SoC는 "M4/AON 도메인이 유발하는, watchdog/pin/software와는 하드웨어적으로 구분되는 저전력 wake"라는 개념을 갖고 있습니다. 즉 **M4가 M55를 깨우는 공식 경로가 하드웨어 차원에서 실제로 존재**하며, 다른 리셋 원인들과 마찬가지로 레지스터 이름 자체가 "reset cause"인 것에서 알 수 있듯 **이 wake도 리셋을 동반**합니다.

다만 이 SDK 버전에는 이 트리거를 애플리케이션 코드가 능동적으로 발생시키는 API나 Kconfig 옵션이 존재하지 않습니다. 이 트리거가 정확히 무엇에 의해 발생하는지 실기로 찾아내는 것이 Lab 04의 목표입니다.

### 3.4 AON Event GPIO는 M4↔M55 통신용이 아니라, 내부 PMU 상태머신을 관찰하기 위한 디버그 전용 출력입니다

근거: `drivers/misc/syna_aon_event_gpio/syna_aon_event_gpio.c`, `include/zephyr/dt-bindings/misc/syna_aon_event_gpio.h`. 노드 `aon_event_gpio: gpio@50350060`(`compatible = "syna,aon-event-gpio"`)는 M55 dtsi에만 존재합니다.

이벤트 정의 헤더에 `AON_GPO2_EVENT_LPPROC_SM_WAKE`(M4 = LPPROC 도메인), `AON_GPO2_EVENT_MAIN_SM_WAKE`(M55 = MAIN 도메인) 등이 정의되어 있어, 이 SoC 내부에 **M4/M55 각각의 전력 도메인을 관리하는 진짜 하드웨어 상태머신(state machine)이 존재**함을 확인할 수 있습니다.

드라이버는 부팅 시 1회 레지스터를 설정(event/pulse-width/polarity)하는 초기화 전용 코드이며, 런타임에 애플리케이션이 읽거나 쓸 수 있는 API가 없습니다. 즉 **소프트웨어로 M4↔M55 신호를 주고받는 용도로는 쓸 수 없습니다.** 다만 오실로스코프나 로직 애널라이저를 이 핀에 연결해두면, MAIN_SM(M55)/LPPROC_SM(M4)이 실제로 언제 WAKE/IDLE 상태를 전환하는지 눈으로 관찰할 수 있는 검증 도구로는 유용합니다(Lab 04에서 보조 수단으로 활용).

### 3.5 보드에 물리적인 "Wake-up" 전용 버튼이 존재하지만, devicetree에는 대응 노드가 없습니다

근거: `boards/syna/astra_sr/sr100/doc/index.rst`에 "Push buttons for system reset and wake-up"이라는 문구가 있습니다. 그러나 devicetree 어디에도 이 버튼에 대응하는 GPIO 노드가 없습니다.

이는 Zephyr가 이미 부팅된 뒤 애플리케이션이 폴링/인터럽트로 읽는 종류의 GPIO가 아니라, **Zephyr 실행 이전, PMU/전원 시퀀싱 단계에서 동작하는 하드웨어 레벨 버튼**일 가능성을 시사합니다. 이 가능성은 4장의 회로도 리뷰에서 뒷받침되는 근거를 확인했습니다.

### 3.6 M4→M55 방향의 전원/리셋 제어 소프트웨어 API는 SDK 전체에 없습니다

`soc/syna/astra_sr/sr100/soc.c`의 `soc_late_init_hook()`에는 `CONFIG_SR100_RELEASE_M4_RESET`(M55가 부팅 후 M4를 reset에서 풀어주는 옵션, `AON_CONFIG` 레지스터의 `(0x3 << 8)` 비트를 클리어)만 존재하고, 이에 대응하는 "M55 release/reset" 옵션은 SDK 어디에도 없습니다(`Kconfig.soc` 전수 확인).

**결론**: "M4가 M55를 깨운다"를 실제 코드로 구현한 사례는 이 SDK에도, 별도로 진행한 M4↔M55 IPC 커리큘럼에도 없습니다 — 이 랩(Lab 00)이 처음으로 다루는 영역입니다.

### 3.7 M4/M55 병렬 빌드 방식

근거: `samples/m4/README.rst`. M4를 `sr100_rdk/sr100/m4`로 별도 빌드한 뒤, M55를 `-DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD=<M4 빌드 경로>`로 빌드해 하나의 flash 이미지로 통합합니다. 이 방식은 별도로 진행한 M4↔M55 IPC 커리큘럼과 동일하며, `M4_BUILD`가 M55 빌드 디렉토리 기준 상대경로로 해석된다는 점(워크스페이스 구조에 따라 `../m4` 등)도 그 커리큘럼에서 이미 확인된 사항입니다.

## 4. SR110 RDK 회로도(SC950-C01116-01 RevE) 리뷰 결과

3장의 SDK 리뷰는 "Zephyr가 부팅된 이후" 소프트웨어 레벨의 사실만 알려줍니다. 하지만 3.5절에서 언급한 물리 wake 버튼처럼, **Zephyr가 실행되기 이전 단계(PMU 전원 시퀀싱)에서만 동작하는 하드웨어 경로**는 회로도를 봐야만 확인할 수 있습니다. 이 절은 SR110 RDK 회로도(13페이지)를 검토한 결과입니다.

### 4.1 조사 방법과 한계

회로도 PDF에서 텍스트를 추출하는 방식으로 넷 이름(net name)과 부품 정보를 확인했습니다. 이 방법은 신호 이름과 부품 목록을 파악하는 데는 유효하지만, **페이지 내 정확한 좌표·배선 연결선(wire routing)까지는 확인하지 못합니다.** 즉 아래 내용은 "이런 신호와 부품이 회로도에 존재하고, 같은 넷 이름으로 여러 페이지에 걸쳐 등장한다"는 사실까지는 확정이지만, "핀 A와 핀 B가 그림상 선으로 직접 이어져 있는지"의 최종 확인은 PDF를 도면으로 직접(픽셀 단위로) 검토하거나 실제 보드에서 멀티미터로 연속성을 재는 방식의 추가 검증이 필요합니다. 이 절의 내용 중 이런 한계가 있는 부분은 7장 TBD 목록에 다시 명시합니다.

### 4.2 `AON_GPI1` / `GPIO3` — SoC 측 AON 입력 핀

- **Page 4** (SR110 메인 SoC 페이지): `U2 SR110_FCCSP122`(SoC 패키지)의 핀 목록에 `SR110_GPIO3.AON_GPI1`이라는 이름의 핀이 있고, 이 신호는 **Page 9**(GPIO Expander 페이지)로 교차 참조(off-page reference)되어 있습니다.
- 이는 3.4절에서 확인한 "AON 상태머신 관련 신호는 M55 devicetree(`aon_event_gpio`)에 출력(GPO) 위주로 노출되어 있다"는 사실과 별개로, **SoC에 AON 도메인으로 들어오는 입력(GPI) 핀도 최소 1개(`AON_GPI1`, GPIO3와 겸용) 존재한다**는 것을 회로도 레벨에서 처음 확인한 것입니다. 이 핀은 SDK의 어떤 devicetree 파일에도 노드로 노출되어 있지 않습니다(3장에서 확인된 노드 목록에 없음).
- **TBD**: 이 핀이 실제로 무엇에 연결되어 M55(또는 M4)에서 관측 가능한지, GPIO3로서 애플리케이션이 읽을 수 있는 범용 GPIO인지, 아니면 AON 하드웨어 로직 전용이라 소프트웨어에서 접근 불가능한지는 이번 텍스트 기반 리뷰만으로 확정하지 못했습니다.

### 4.3 Wake-up 트리거 회로 (Page 13) — 버튼 + RTC + PIR → `PMU_EN`

**Page 13**은 회로도 자체에 "RTC, Wake-up"이라는 섹션 제목이 붙어 있고, "Wake up triggers"라는 명시적인 문구가 포함된 페이지입니다. 이 페이지에서 확인한 부품과 신호는 다음과 같습니다.

| 부품/신호 | 설명 |
|---|---|
| `U26 BU9873NUX-TTR` | I2C 인터페이스 RTC(Real-Time Clock) IC. I2C 주소 `0x32`. |
| `RTC_SCL` / `RTC_SDA` | RTC의 I2C 버스 신호 |
| `RTC_INTn` | RTC의 인터럽트 출력(active-low로 추정, 신호명의 `n` 접미사 기준) |
| `RTC_INT_HI-ACT` | RTC 인터럽트를 active-high로 변환한 신호로 보이는 별도 넷 |
| `PIR_INT` | PIR(적외선 동체감지) 센서의 인터럽트 신호 |
| `SW7` (Tact_SPST_NO) | 순간접촉식 푸시버튼. Page 2(전원 트리 페이지)에도 동일 부품이 나타나, 3.5절에서 SDK 문서상으로만 확인했던 "물리 Wake-up 버튼"이 바로 이 `SW7`일 가능성이 높은 것으로 판단됩니다. |
| `EXT_INT` | 위 신호들이 합쳐지는(설명 텍스트상 병렬 결선으로 보이는) 지점으로 확인되며, 최종적으로 `SR110_PMU_EN` 신호를 구동하는 것으로 표기되어 있습니다. |
| `SR110_PMU_EN` | SoC의 PMU(Power Management Unit) Enable 입력으로 추정되는 신호 |

**해석**: 이 페이지는 "버튼(SW7)을 누르거나, RTC 알람이 울리거나(`RTC_INTn`), PIR 센서가 움직임을 감지하면(`PIR_INT`), 그 신호들이 하나의 지점(`EXT_INT`)으로 모여 SoC의 `PMU_EN`을 구동한다"는 구조로 보입니다. `PMU_EN`이라는 이름 자체가 "PMU를 켠다"는 의미이므로, 이는 **Zephyr가 실행되기 전, 아직 SoC에 전원이 완전히 들어오지 않았거나 PMU가 저전력 상태에 있는 시점에 전원을 (다시) 켜는 경로**로 해석하는 것이 SDK 리뷰(3.5절)와도 일치합니다.

### 4.4 Power Tree(Page 2)의 `AON_GPO1`

**Page 2**(전원 트리 페이지)에 "`JP1` `AON_GPO1` BJT R System Power selection npn"이라는 문구가 있어, SoC의 `AON_GPO1` 출력이 BJT(NPN 트랜지스터)를 거쳐 시스템 전원 선택/스위칭에 관여하는 것으로 보입니다. 같은 페이지에 메인 전원 슬라이드 스위치 `SW1`과 buck-boost 컨버터 `TPS63900DSKR`도 확인됩니다.

이 신호(`AON_GPO1`)는 3.4절에서 확인한 `aon_event_gpio` 드라이버가 초기화하는 GPO 핀들(`AON_GPO0/1/2`) 중 하나와 이름이 일치합니다. 즉 **SDK에서는 "디버그 관찰용 출력"으로만 파악했던 이 핀이, 회로도상에서는 실제로 전원 스위칭 트랜지스터를 구동하는 실질적인 역할을 겸하고 있을 가능성**이 있습니다. **TBD**: `aon_event_gpio` 드라이버의 이벤트 설정(예: `AON_GPO2_EVENT_MAIN_SM_WAKE` 등)이 이 전원 스위칭 동작과 실제로 연동되는지, 아니면 이 BJT 경로는 완전히 별개의 정적인 전원 시퀀싱 회로인지는 이번 리뷰로 확정하지 못했습니다.

## 5. 종합 — M4가 M55를 깨우는 경로 정리

이전 대화에서 세 가지를 확인해 달라는 질문을 주셨습니다. SDK 리뷰만으로 답했던 내용을, 이번 회로도 리뷰 결과를 반영해 아래와 같이 정정/보강합니다.

| # | 질문 | 회로도 리뷰 이전 답변 | 회로도 리뷰 반영 후 |
|---|---|---|---|
| 1 | M4→M55를 GPIO 인터럽트로 깨우는 방법은 없는가? | 없는 것으로 보임(devicetree에 해당 경로 없음) | **정정**: `AON_GPI1`(4.2절)이라는, devicetree에 노출되지 않은 SoC 입력 핀이 회로도에 존재합니다. "없다"고 단정할 수 없고, "SDK/devicetree 레벨에서는 소프트웨어로 접근하는 경로가 없다"는 것까지만 확정입니다. |
| 2 | M4→M55를 깨우는 방법은 IPC(mbox)가 유일한가? | 소프트웨어로 검증된 유일한 경로 | **정정**: "지금까지 M4↔M55 IPC 커리큘럼에서 실기로 검증하고 실제로 쓰고 있는, 소프트웨어 레벨의 유일한 경로"라는 의미로 한정해야 합니다. 하드웨어에는 4.3절의 `PMU_EN` 경로처럼 mbox와 무관한 별도의 물리적 wake 경로가 최소 1개 더 존재합니다. |
| 3 | M55가 자체적으로 깨어나는 방법은 wake-up 버튼과 watchdog뿐인가? | 버튼 + watchdog 2가지로 추정 | **정정 및 확장**: 최소 4가지 트리거가 확인됩니다 — ① 버튼(`SW7`), ② RTC 알람(`RTC_INTn`, `U26 BU9873NUX`), ③ PIR 센서(`PIR_INT`), ④ M55 watchdog(3.2절). 이 중 ①~③은 회로도상 같은 지점(`EXT_INT`)에서 합쳐져 하나의 `PMU_EN` 경로를 공유하는 것으로 보이는 반면, ④는 이들과 전혀 다른 메커니즘(6장 참고)입니다. |

## 6. 서로 다른 두 종류의 "리셋"을 구분해야 합니다

이번 조사에서 가장 중요한 결론은, SR110에서 "M55가 다시 시작한다"는 현상이 **성격이 다른 최소 두 가지 메커니즘**으로 발생할 수 있다는 점입니다.

1. **`PMU_EN` 기반 콜드 파워온** (4.3절): 버튼/RTC/PIR가 `EXT_INT`를 통해 SoC의 PMU 자체를 켜는 경로입니다. 이는 Zephyr나 워치독 레지스터와 무관한, **SoC에 전원이 아예 새로 들어오는(또는 저전력 상태의 PMU가 다시 활성화되는) 물리적 전원 시퀀싱**에 가깝습니다. `hwinfo_get_reset_cause()`가 이를 어떻게 분류하는지(`RESET_POR`인지 `RESET_LOW_POWER_WAKE`인지)는 아직 실기로 확인하지 못했습니다.
2. **watchdog 기반 SoC 리셋** (3.2절): SoC에 이미 전원이 들어와 있는 상태에서, M55(또는 M4) 워치독 타임아웃이 `AON_POR_RST` 레지스터를 통해 SoC 리셋을 발생시키는 경로입니다. `hwinfo_get_reset_cause()`가 `RESET_WATCHDOG`로 명확히 분류합니다(3.2/3.3절, SDK 소스로 확정).

두 경로 모두 "M55가 `main()`부터 다시 시작한다"는 결과는 같지만, 하나는 **PMU/전원 레벨의 이벤트**이고 다른 하나는 **이미 켜진 SoC 내부의 소프트웨어적 리셋**이라는 점에서 원인이 전혀 다릅니다. 이 구분을 명확히 하는 것이 Lab 02(watchdog)와 Lab 04(PMU_EN 계열 wake 탐색)를 분리해서 설계한 이유입니다.

## 7. 확인 필요 사항 (TBD, 실기 검증 필요)

1. `RESET_LOW_POWER_WAKE`를 실제로 무엇이 트리거하는지 — `AON_CONFIG` 레지스터의 미문서화된 비트, PMU 시퀀싱, 혹은 4.3절의 `PMU_EN` 경로와의 관계 (Lab 04에서 탐색)
2. `SW7` 버튼이 devicetree상 노출된 어떤 GPIO도 아니라는 점에서, 이 버튼을 눌렀을 때 Zephyr 애플리케이션 관점에서 `hwinfo_get_reset_cause()`가 어떤 값을 보고하는지 (`RESET_PIN`, `RESET_POR`, `RESET_LOW_POWER_WAKE` 중 하나로 예상되나 미확인)
3. `AON_GPI1`(4.2절)이 실제로 M4 또는 M55에서 소프트웨어로 접근 가능한 범용 GPIO인지, 아니면 AON 하드웨어 로직 전용이라 접근 불가능한지
4. `AON_GPO1`(4.4절)이 전원 스위칭 트랜지스터를 구동하는 것이 `aon_event_gpio` 드라이버의 이벤트 설정과 실제로 연동되는지, 아니면 완전히 정적인 회로인지
5. RTC(`U26 BU9873NUX`) 알람과 PIR 센서(`PIR_INT`)가 실제로 이 보드에 실장/연결되어 있는지, 아니면 회로도상에는 존재하나 RDK 조립 옵션에 따라 미실장일 수 있는지 (부품 실장 여부는 회로도만으로 확정 불가)
6. M55 watchdog(`wdog0`)이 base dtsi에서 `status = "disabled"`이므로, 실제 보드 오버레이에서 `okay`로 켜본 적이 이 프로젝트에서 아직 없음 — Lab 02에서 처음 시도
7. `WDT_SYNA_DEFAULT_TIMEOUT_MAX`(기본 1000ms) 외에 실제로 설정 가능한 timeout 범위, 그리고 "첫 만료(콜백) ~ 두 번째 만료(리셋)" 사이의 실측 시간
8. 4.1절에서 밝힌 한계대로, 이 장의 신호 연결 관계는 텍스트 추출 기반 1차 확인이며 회로도 도면 자체를 픽셀 단위로 재확인하거나 실제 보드에서 멀티미터로 연속성을 재는 검증이 아직 이루어지지 않았습니다.

## 8. 다음 랩 예고 (Lab 01)

`CONFIG_PM`이 구현되어 있지 않으므로(3.1절), Lab 01은 Zephyr 커널이 기본으로 제공하는 idle thread의 자동 WFI 진입만으로 baseline을 잡습니다. M4는 항상 켜진 채로 고정된 워크로드(heartbeat)를 유지하고, M55만 ACTIVE(CPU가 계속 ready 상태를 유지해 WFI가 전혀 실행되지 않는 모드)와 IDLE(짧은 작업 사이사이 `k_msleep()`으로 대기해 idle thread가 WFI를 실행하는 모드) 사이를 온보드 버튼(SW8)으로 오가며 멀티미터로 전류 차이를 실측합니다.

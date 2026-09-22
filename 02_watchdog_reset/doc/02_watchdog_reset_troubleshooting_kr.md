# Lab 02 트러블슈팅

## 이슈 1 — `WDT_FLAG_RESET_NONE`이 "무한루프로 정지"가 아니라 "콜백만 계속 반복" (2026-09-21)

### 증상

Lab 00의 소스 리뷰를 근거로, `WDT_FLAG_RESET_NONE`으로 설정하고 feed를 멈추면 콜백이 한 번 호출된 뒤 드라이버가 내부적으로 무한루프에 빠져 시스템이 완전히 정지할 것이라고 예상하고 Lab 02의 Step 1을 작성했습니다. 그런데 실기에서 관찰된 결과는 이와 달랐습니다.

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

콜백이 한 번만 찍히고 멈추는 것이 아니라, **`WDT_TIMEOUT_MS`(2000ms)와 거의 정확히 일치하는 주기로 계속 반복해서 호출**되고 있습니다(로그의 콜백 발생 시점 간격을 재보면 약 2000~2024ms). 그리고 그 사이 `main()`의 heartbeat 로그(`[main] still running, ...`)도 250ms 주기로 전혀 끊기지 않고 계속 찍히고 있습니다 — 즉 **hang도 없고, 리셋도 없이, 시스템이 완전히 정상 동작을 계속 유지**하고 있었습니다.

### 진단 — Lab 00 소스 리뷰의 추정이 실기로 반증됨

Lab 00 리뷰 당시에는 "`RESET_NONE`이면 feed 안 했을 때 두 번째 만료 시점에 무한루프에 빠진다"고 결론 내렸으나, 이는 소스 코드를 읽고 세운 **추정**이었을 뿐 이 프로젝트에서 실기로 검증된 적은 없었습니다. 이번 Lab 02가 그 추정을 처음으로 실기 테스트한 것이고, 결과는 추정과 달랐습니다.

실기 결과를 가장 잘 설명하는 해석은 다음과 같습니다: 이 드라이버는 (적어도 콜백이 등록되어 있고 `RESET_NONE`인 경우) 매 timeout 만료마다 **콜백을 호출한 뒤 하드웨어 카운터를 자동으로 재적재(재무장)**하는 것으로 보입니다 — 애플리케이션이 `wdt_feed()`를 호출했는지 여부와 무관하게 말입니다. 그 결과 `RESET_NONE`으로 설정한 워치독은 사실상 "리셋도, hang도 일으키지 않으면서 일정 주기로 콜백만 반복 호출하는 소프트웨어 타이머"처럼 동작합니다. 인터럽트를 클리어하는 과정에서 카운터도 함께 재적재되는 것이 일반적인 워치독 하드웨어/드라이버 설계 패턴이라는 점을 감안하면, 오히려 이쪽이 "무한루프로 정지"보다 더 흔하고 합리적인 설계입니다.

**중요한 함의**: 이 SDK에서 `WDT_FLAG_RESET_NONE`은 "타임아웃 시 알림만 받고, 실제 안전장치(리셋)는 없는" 순수 로깅/모니터링 용도로만 써야 합니다 — feed를 잊어도 시스템이 멈추거나 재시작되지 않으므로, **진짜 장애 복구(hang 탈출) 목적의 안전장치가 필요하다면 반드시 `WDT_FLAG_RESET_SOC`를 써야 합니다.**

### 조치

1. `main.c`의 콜백 함수에 `callback_count`(호출 횟수) 카운터를 추가해, 콜백이 반복 호출되고 있다는 사실이 로그만 봐도 명확히 드러나도록 했습니다.
2. 콜백 안의 안내 메시지를 "hang이 예상된다"에서 "이 콜백이 계속 반복될 것으로 예상된다"로 정정했습니다.
3. 파일 상단 설계 노트와 레퍼런스 문서(`02_watchdog_reset_kr.md`)의 "Lab 00/01과의 연결", "코드 구성", "Step 1" 절을 이번 실기 결과에 맞춰 갱신했습니다.
4. **`WDT_FLAG_RESET_SOC`가 실제로 리셋을 일으키는지는 아직 확인되지 않았습니다** — `RESET_NONE`에 대한 사전 추정이 이미 한 번 틀렸으므로, `RESET_SOC`에 대한 "약 2배 시점에 리셋" 추정도 검증 전까지는 가설로만 취급합니다. Step 2 실기 테스트 결과가 나오면 이 문서에 반영하겠습니다.

### 남은 TBD

- 콜백 호출 후 카운터가 자동으로 재적재되는 정확한 메커니즘(드라이버 코드 경로)은 소스를 다시 들여다보지 않는 한 특정할 수 없습니다 — 이번 실기 결과로 이 프로젝트 범위에서는 더 파고들지 않습니다.
- `WDT_FLAG_RESET_SOC`에서도 이번과 같은 자동 재무장이 일어나 리셋 자체가 발생하지 않을 가능성을 배제할 수 없습니다. 이 경우 이 랩의 원래 학습 목표("feed를 멈추면 정말 리셋되는가")는 "이 SDK에서는 `RESET_SOC`가 아니면 리셋 안전장치로 쓸 수 없다"는 형태로 결론이 바뀔 수 있습니다.


## 이슈 2 — `WDT_FLAG_RESET_SOC`가 콜백 이후 SoC 전체를 정지시키고, 스스로 재부팅하지 않음 (2026-09-21)

### 증상

Step 2(`WDT_TEST_USE_RESET_SOC=1`) 테스트에서, 콜백은 정상적으로 한 번 호출되었으나 그 이후 아무 로그도 더 이상 찍히지 않았습니다. 사용자가 제공한 로그는 콜백 발생 메시지에서 그대로 끊깁니다:

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

(이후 로그 없음 — 재부팅 배너("`*** Booting Zephyr OS ***`")가 다시 뜨지 않음)

사용자가 이후 직접 확인한 두 가지 추가 사실:

1. **물리 리셋 버튼을 누르면 다시 정상적으로 부팅됨** — 즉 보드 자체나 디버거/UART 연결이 완전히 죽은 것은 아니고, 정상적으로 리셋을 받아들일 수 있는 상태.
2. **멈춘 상태에서는 M4의 로그도 함께 멈춤** — M55 콘솔뿐 아니라, 완전히 별도의 코어이자 별도의 UART로 로그를 찍고 있던 M4(`lab/remote/src/main.c`)의 heartbeat도 같은 시점에 정지함.

### 진단 — "M55만의 로컬 리셋"이라는 가정이 실기로 반증됨

Lab 00/01/이 문서의 원래 설계 의도는 "`WDT_FLAG_RESET_SOC`는 M55 코어만 리셋시키고, Always-On인 M4는 영향을 받지 않는다"는 것이었고, `lab/remote/src/main.c`도 그 가정을 검증하기 위한 용도로 작성되었습니다(M4가 계속 tick을 찍으면 M55만 리셋됐다는 증거가 될 것이라는 의도). 하지만 실기 결과는 이 가정과 정반대입니다: **콜백 발생 이후 M55와 M4가 동시에, 완전히 정지**하며, 이는 M55만의 국소적 리셋이 아니라 **SoC 전체(두 코어 모두)를 대상으로 하는 리셋 시도**임을 시사합니다.

더 나아가, 이 정지 상태가 **스스로 재부팅으로 이어지지 않는다**는 점이 중요합니다. 만약 이것이 정상적으로 완료되는 SoC 리셋이었다면 잠시 후 두 코어 모두에서 부팅 배너가 다시 찍혀야 하는데, 실제로는 물리 리셋 버튼을 누르기 전까지 무기한 정지 상태로 남아 있었습니다. 이는 다음 두 가지 해석 중 하나로 설명됩니다:

- **(A) 리셋이 "래치/스턱(latched/stuck)"된 경우**: 워치독이 리셋 신호를 발생/래치시켰지만 그 리셋 시퀀스가 하드웨어적으로 끝까지 완료되지 못하고 대기 상태에 걸려 있다가, 물리 리셋 핀이 눌리면서 (이미 걸려 있던 리셋 요청과 결합하여, 또는 그것과 무관하게) 비로소 리셋이 완료된 경우.
- **(B) 리셋이 사실상 아무 효과도 없었던 경우**: 워치독의 SoC 리셋 시도 자체가 이 SDK/보드 조합에서는 실질적으로 아무 것도 완료하지 못한 채 시스템만 멈춰 세우고, 물리 리셋 버튼이 완전히 독립적으로 정상 리셋 시퀀스를 수행해 복구된 경우.

이 두 해석을 구분할 수 있는 유일한 단서는 **물리 리셋 버튼을 누른 후 부팅되는 로그에서 `hwinfo_get_reset_cause()`가 어떤 비트마스크를 보고하는가**입니다. `RESET_WATCHDOG` 비트가 함께 찍힌다면 (A)에 가깝고(워치독 리셋 요청이 어떤 형태로든 유지되어 있다가 이번 부팅 원인으로 기록됨), `RESET_PIN`만 찍히고 `RESET_WATCHDOG`는 전혀 없다면 (B)에 가깝습니다(워치독 리셋 시도는 흔적 없이 사라지고, 이번 부팅은 순전히 핀 리셋의 결과). 이 정보는 아직 확인되지 않았습니다 (남은 TBD 참고).

한 가지 참고할 배경: 이 드라이버는 애초에 `WDT_FLAG_RESET_CPU_CORE`(코어 단위 리셋)를 `-EINVAL`로 거부하고 `WDT_FLAG_RESET_NONE`과 `WDT_FLAG_RESET_SOC` 두 가지만 지원합니다. 이번 실기 결과는 그 설계와 일관됩니다 — 애초에 "코어 하나만" 리셋하는 옵션 자체가 없었다는 것은, 이 워치독 하드웨어가 처음부터 SoC 전체 단위로만 리셋을 걸 수 있게 설계되어 있음을 시사하며, `RESET_SOC`가 이름 그대로 "SoC 전체"를 대상으로 하는 것은 오히려 자연스러운 결과입니다. 다만 그 리셋이 "스스로 완료되지 않는다"는 점은 이 워치독 특유의 동작인지, 아니면 이 보드/SDK 조합에서만 나타나는 문제인지는 아직 알 수 없습니다.

**커리큘럼 전체에 영향을 주는 중요한 정정**: Lab 00부터 반복적으로 설명해 온 "M4는 Always-On이라 M55의 전원 상태 변화와 무관하다"는 명제는, 적어도 `WDT_FLAG_RESET_SOC` 트리거 상황에서는 **성립하지 않습니다**. M4가 M55의 IDLE/WFI(Lab 00/01에서 확인)와는 무관하게 계속 동작하는 것은 여전히 사실이지만, M55의 워치독이 SoC 리셋을 시도하는 순간에는 M4도 함께 멈춥니다. 이후 IPC(Lab 03)나 Low-Power-Wake 관련 랩을 설계할 때, "M4는 항상 살아있다"는 가정을 무조건 재사용하지 말고 어떤 종류의 M55 상태 변화인지(전원 모드 vs. 리셋)를 구분해서 설명해야 합니다.

### 실기 확인 결과 업데이트 — 물리 리셋 후 reset cause는 `RESET_WATCHDOG`도 `RESET_PIN`도 아닌 `RESET_POR` (2026-09-21)

멈춘 상태에서 물리 리셋 버튼을 누른 직후 부팅 로그:

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

앞서 세운 (A)/(B) 두 가설은 모두 "`RESET_WATCHDOG`가 찍히느냐 `RESET_PIN`만 찍히느냐"를 기준으로 했는데, 실제로는 **둘 다 아니고 `RESET_POR`(Power-On-Reset)이 찍혔습니다.** 이는 원래 예상 밖의 세 번째 결과이며, 다음 두 가지를 함께 시사합니다:

1. **워치독의 SoC 리셋 시도는 이번 부팅 원인에 전혀 흔적을 남기지 않았습니다** — `RESET_WATCHDOG` 비트가 전혀 없으므로, (A)("리셋이 래치되어 있다가 물리 리셋과 함께 완료됨")는 배제됩니다. 이는 사용자가 제기한 가설 — **"Watchdog reset은 (하드웨어적으로) 셋팅이 안 되는 것 같다"** — 와 정확히 부합하는 결과입니다: `wdt_install_timeout()`에 `WDT_FLAG_RESET_SOC`를 넘겨도 실제 리셋-인에이블 레지스터 비트가 제대로 프로그래밍되지 않는 드라이버 버그일 가능성이 높습니다.
2. **다만 이 결과만으로 (B)("물리 리셋이 완전히 독립적으로 복구")를 확정할 수도 없습니다** — `RESET_PIN`이 아니라 `RESET_POR`이 찍혔다는 것은, 이 보드의 "System Reset" 물리 버튼 자체가 단순 핀 리셋이 아니라 **PMU/전원단까지 내려갔다 올라오는 완전한 파워온리셋(POR) 시퀀스**를 유발하는 방식으로 설계되어 있을 가능성을 보여줍니다(1-2절 ⑤에서 확인한 "System Reset과 Wake-up이 별도 버튼"이라는 사실, 그리고 PMU_EN 기반 콜드 파워온 경로의 존재와 궤를 같이합니다). 만약 이 버튼이 **watchdog 테스트와 무관하게 항상** `RESET_POR`을 보고한다면, 이번 관찰은 애초에 (A)/(B)를 가를 수 있는 정보가 아니게 됩니다.

**baseline 확인 결과 (2026-09-21) — 최종 결론**: 이 랩과 완전히 무관한 정상 동작 중에 같은 물리 리셋 버튼을 눌러봐도 동일하게 `reset cause bitmask = 0x00000008 -> RESET_POR`이 나왔습니다. 즉 **이 보드의 물리 "System Reset" 버튼은 watchdog 여부와 상관없이 항상 `RESET_POR`을 보고하는 버튼**입니다(1-2절 ⑤의 PMU_EN 기반 콜드 파워온 경로와 같은 계열로 추정 — 단순 핀 신호가 아니라 PMU까지 내려갔다 올라오는 리셋을 유발하는 것으로 보입니다).

이로써 (A)/(B) 가설을 `hwinfo_get_reset_cause()`로 가르려던 진단 방법은 **막다른 길임이 확인되었습니다**: 이 버튼으로 복구하는 한, 그 직전에 워치독이 무슨 짓을 했든 reset_cause 레지스터는 항상 POR로 덮어써지므로 워치독의 SoC 리셋 시도가 실제로 하드웨어에 반영됐는지 여부를 이 방법으로는 원천적으로 알 수 없습니다. (A)("리셋이 래치되어 있었다")와 (B)("워치독 리셋은 흔적 없이 사라졌다") 둘 다 이 관찰과 모순되지 않으므로, 이 질문은 이 프로젝트의 도구(멀티미터, 콘솔 로그)로는 더 이상 좁힐 수 없는 것으로 결론짓습니다. 더 깊이 파려면 디버거로 워치독 레지스터를 직접 read/write하며 관찰하거나, 리셋 라인 자체를 오실로스코프로 관찰해야 할 것으로 보이나, 이는 이 커리큘럼의 범위를 벗어납니다.

사용자가 제기한 "watchdog reset이 하드웨어적으로 셋팅이 안 되는 것 같다"는 가설은 여전히 유력한 후보로 남지만, 위 이유로 이 프로젝트에서 확정할 수는 없습니다.

### 조치

1. `02_watchdog_reset_kr.md`의 "Lab 00/01과의 연결", "하드웨어 구성", "Step 2", "남은 TBD" 절을 이번 실기 결과에 맞춰 갱신했습니다.
2. `lab/src/main.c`(M55)와 `lab/remote/src/main.c`(M4)의 파일 상단 설계 노트를 이번 결과에 맞춰 정정했습니다 — 특히 M4 쪽의 "M55의 워치독 리셋은 M55만의 로컬 리셋임을 증명한다"는 기존 설명이 정반대로 뒤집혔음을 명시했습니다.
3. M55 콜백의 RESET_SOC 분기 로그 메시지를 "리셋이 될 수도 있다(미확인)"에서 "SoC 전체(M55+M4)가 멈추며, 스스로 복구되지 않으니 물리 리셋이 필요하다"로 정정했습니다.

### 남은 TBD

- ~~물리 리셋 버튼을 누른 직후 부팅에서 `hwinfo_get_reset_cause()`가 보고하는 비트마스크~~ — **확인 완료(2026-09-21): 항상 `RESET_POR`(`0x00000008`)이며, watchdog 여부와 무관한 이 버튼 고유의 특성임을 baseline 테스트로 확인**. `hwinfo_get_reset_cause()` 기반 진단은 이 이유로 (A)/(B)를 가를 수 없다는 것이 최종 결론입니다(위 절 참고).
- 이 "멈춘 뒤 스스로 복구되지 않는" 동작이 `WDT_TIMEOUT_MS` 값(2000ms)에 따라 달라지는지, 더 긴 타임아웃에서는 다르게 동작하는지는 확인하지 않았습니다 — 이 프로젝트 범위에서는 더 조사하지 않기로 합니다.
- `wdt_install_timeout()`/`wdt_setup()`에 `WDT_FLAG_RESET_SOC`를 넘겼을 때 실제로 리셋-인에이블 레지스터 비트가 프로그래밍되는지는, 콘솔 로그·reset_cause만으로는 확정할 수 없는 것으로 결론지었습니다(위 절 참고) — 디버거를 통한 레지스터 직접 관찰이 필요하나 이 프로젝트 범위 밖입니다.
- **최종 결론**: 이 SDK/보드 조합에서 `WDT_FLAG_RESET_SOC`는 자체적으로 완결되는 안전장치型 리셋으로 **확인되지 않았습니다** — feed를 멈추면 콜백은 정확히 한 번 발생하지만, 그 이후 시스템(M55+M4 전체)이 정지하고 물리적 개입 없이는 복구되지 않습니다. 그 근본 원인(하드웨어 리셋이 실제로 트리거됐으나 완료되지 못한 것인지, 아니면 애초에 트리거되지 않은 것인지)은 이 프로젝트의 도구로는 확정할 수 없었습니다. 실제 제품에 이 워치독을 안전장치로 채택하려면, 이 결과를 바탕으로 Synaptics에 별도 확인이 필요합니다.

# SR110 Zephyr Power Mode 실습 커리큘럼

Synaptics **SR110 (Astra Machina, SR100 SoC)** RDK 보드에서, Cortex-M55(메인 코어)와 Cortex-M4(Always-On 코어) 사이의 전원 관리(Power Management)와 wake 메커니즘을 Zephyr OS(`syna_zephyr_sdk-1.0.0`)로 실기 검증하는 실습/세미나용 커리큘럼입니다.

## 이 커리큘럼의 핵심 질문

- M4(Always-On)가 M55(전원 상태가 바뀌는 코어)를 깨우는 공식적인 방법은 무엇인가?
- M55가 깨어난 뒤 멈췄던 지점에서 이어서 실행되는가(WFI), 아니면 처음부터 다시 부팅되는가(리셋)?
- SR110이 하드웨어적으로 지원하는 것으로 보이는 Active / Low-power / Ultra-low-power mode를, 이 SDK 버전에서 소프트웨어로 실제 다룰 수 있는가?

세 번째 질문에 대한 답은 미리 말씀드리면 **"이 SDK 버전에는 그 API가 없다"** 입니다 — 커리큘럼 마무리(Lab 05)와 아래 "총평" 절 참고.

## 폴더 구조

각 랩은 `NN_주제/` 폴더 하나이며, 안에 코드(`lab/`)와 문서(`doc/`)가 함께 들어 있습니다.

```
NN_주제/
  lab/
    src/main.c          # M55 코드
    remote/src/main.c   # M4 코드
    boards/*.overlay    # devicetree 오버레이 (필요한 랩만)
    CMakeLists.txt, prj.conf (M55/M4 각각)
  doc/
    NN_주제_kr.md                     # 한글 강의 문서 (원본)
    NN_주제_en.md                     # 영문 강의 문서 (한글 검토 후 번역)
    NN_주제_troubleshooting_kr.md     # (있는 경우) 실기 디버깅 중 발견된 이슈 상세
```

빌드는 모든 랩이 동일한 방식입니다 — M4를 먼저 빌드하고, 그 결과물을 M55 빌드에 패키징합니다.

```bash
west build -p always -b sr100_rdk/sr100/m4 -d m4 NN_주제/lab/remote
west build -p always -b sr100_rdk/sr100/m55 -d m55 NN_주제/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

## 랩 목록

| 랩 | 제목 | 상태 | 한 줄 요약 |
|---|---|---|---|
| [Lab 00](00_sdk_and_schematic_review/doc/00_sdk_and_schematic_review_kr.md) | SDK/회로도 소스 리뷰 | 완료 (영문 포함) | 이 SDK엔 `CONFIG_PM`이 없고, 회로도상 버튼/RTC/PIR이 `PMU_EN`을 공유한다는 것을 확인 |
| [Lab 01](01_baseline_active_idle/doc/01_baseline_active_idle_kr.md) | Active vs Idle(WFI) 유휴전류 baseline | 완료 (영문 포함) | 온보드 버튼으로 M55를 ACTIVE/IDLE 전환하며 idle thread 자동 WFI의 전류 절감 효과를 실측 |
| [Lab 02](02_watchdog_reset/doc/02_watchdog_reset_kr.md) | M55 자체 Watchdog 리셋 | 완료 (영문 포함) | `WDT_FLAG_RESET_SOC`가 자체 완결되지 않고 물리 개입 없이는 복구 안 되는 정지를 유발함을 확인 |
| [Lab 03](03_ipc_sensor_wake/doc/03_ipc_sensor_wake_kr.md) | M4→M55 IPC(mbox) 기반 wake | 완료 (영문 포함) | M4 가속도계 상태 변화를 mbox로 notify — 리셋 없이 M55가 깨어나는, 유일하게 매끄럽게 동작한 경로 |
| [Lab 04](04_low_power_wake/doc/04_low_power_wake_kr.md) | `SW7`/`PMU_EN`/`RESET_LOW_POWER_WAKE` 탐색 | 완료 (영문 포함) | SoC가 켜진 상태에서는 `SW7`이 무반응 — 저전력 모드 진입 API 자체가 없어 범위 밖 |
| [Lab 05](05_reset_cause_design_guide/doc/05_reset_cause_design_guide_kr.md) | 종합: 리셋 원인 기반 설계 가이드 | 완료 | 부트 원인 디스패치 + Lab 03의 mbox 패턴을 합친 대표 코드, Lab 01~04 종합 비교표 |

## 총평 — 이 SDK/보드 조합에서 확인된 것

- **컨텍스트를 보존한 채 깊이 자는 중간 단계가 없습니다.** WFI(얕음, 즉시 재개) 아니면 리셋(깊음, 완전 재부팅) 두 단계뿐입니다.
- **M4→M55 wake의 공식적이고 확실하게 동작하는 방법은 mbox(Lab 03)뿐입니다.** `RESET_LOW_POWER_WAKE`/`PMU_EN` 경로(Lab 04)는 하드웨어 레벨 존재는 확인되나 소프트웨어로 구동할 방법이 없습니다.
- **watchdog 기반 리셋(Lab 02)은 안전장치로 신뢰하기 어렵습니다.** 트리거되면 정지 상태가 되고, 자동 복구를 보장하지 못합니다.

## 아쉬운 점 (솔직한 회고)

이 커리큘럼을 시작할 때의 목표는 SR110이 하드웨어적으로 지원하는 것으로 보이는 **Active / Low-power / Ultra-low-power mode** 전환을 실습으로 직접 다뤄보는 것이었습니다. 하지만 Lab 00~04로 확인했듯, 이 SDK 버전(`syna_zephyr_sdk-1.0.0`)에는 그 모드들로 진입시키는 소프트웨어 API 자체가 없습니다(`CONFIG_PM` 부재). 그래서 결과적으로 이번에 실습으로 다룰 수 있었던 것은 Zephyr/소프트웨어 레벨의 **Idle → Sleep(WFI) → 다시 Active로 전환하는 정도**(Lab 01)와, 리셋을 동반하는 두 가지 경로(watchdog 리셋, PMU_EN 계열)뿐이었습니다. 특히 안전장치로 기대했던 **watchdog 기반 power reset조차 자체 완결되는 형태로는 동작하지 않는다는 것**이 확인되어(Lab 02), 이번 커리큘럼만으로는 "진짜 저전력 모드 전환"이라는 원래 목표에는 이르지 못했습니다. 이 하드웨어 레벨 저전력 모드를 실제로 여는 방법은 Synaptics SDK의 향후 버전이나 별도 문의를 통해서만 확인 가능할 것으로 보입니다.

## 참고

- 각 랩 문서는 한글로 먼저 작성되고 검토 후 영문으로 번역됩니다.
- 실기 디버깅 중 발견된 SDK/드라이버 이슈 중 중요한 것은 랩별 `*_troubleshooting_kr.md` / `*_troubleshooting_en.md`에 별도 정리되어 있습니다(Lab 01, Lab 02).
- 이 커리큘럼의 설계 배경, 의사결정 과정, 실기 확인 이력의 전체 기록은 Claude Project의 마스터 문서(`sr110-power-mode-curriculum-draft.md`)에 있습니다.

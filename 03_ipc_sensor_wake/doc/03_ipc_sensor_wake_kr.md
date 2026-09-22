# Lab 03 — M4 가속도계 + mbox: 리셋 없이 "실시간으로 깨우기"

## 학습 목표

- Zephyr 표준 mbox API(`mbox_dt_spec`, `mbox_register_callback_dt()`, `mbox_set_enabled_dt()`, `mbox_send_dt()`)로 M4 → M55 단방향 알림 채널을 구성합니다.
- M4에서 온보드 가속도계(`mc3479`, I2C1)를 표준 Zephyr 센서 API(`sensor_sample_fetch()`/`sensor_channel_get()`)로 읽고, 부팅 시 베이스라인(중력/장착 오프셋)을 캘리브레이션하는 패턴을 익힙니다.
- "매 샘플마다 보내기"가 아니라 **상태가 실제로 바뀔 때만 보내는(send-on-change)** 설계로 알림 스팸을 피하는 방법을 익힙니다.
- mbox의 rx 콜백이 **ISR 컨텍스트**에서 실행된다는 것, 그리고 그 안에서는 절대 블로킹 호출(`k_msleep`, `mbox_send_dt()` 포함)을 해서는 안 되고 `k_msgq`로 즉시 넘긴 뒤 리턴해야 한다는 규칙을 실습으로 체감합니다.
- Lab 01(WFI, 타이머로 깨어나 멈췄던 지점 재개)·Lab 02(watchdog, 리셋되어 처음부터 재시작) 이후 **세 번째 종류의 "깨어남"** — M55가 죽지도, 리셋되지도 않고 계속 살아있는 채로, 다른 코어의 메시지가 도착하는 순간에만 반응하는 패턴을 확인합니다.

## 이전 랩과의 연결

Lab 01은 M55가 스스로 짧게 잠들었다가(WFI) 타이머로 깨어나는 "얕은 절전"을, Lab 02는 M55가 완전히 멈췄다가 리셋으로 처음부터 재시작하는 "깊은 절전"을 다뤘습니다. 이번 Lab 03은 그 둘과 또 다른 세 번째 패턴입니다.

- M55는 리셋되지도, 스스로 타이머를 걸지도 않습니다. `k_msgq_get(..., K_FOREVER)`로 **무기한 블로킹**하면서 M4로부터의 메시지만 기다립니다.
- 이 상태에서도 Zephyr의 idle 스레드가 자동으로 WFI에 들어갑니다 — Lab 01과 동일한 메커니즘이지만, 깨우는 주체가 "미리 정해둔 타이머 만료"가 아니라 **다른 코어가 보낸 실시간 메시지**라는 점이 다릅니다.
- 즉 "리셋 없이 깨어나기"라는 점은 Lab 01과 같지만, 깨우는 트리거가 자기 자신의 타이머가 아니라 **다른 코어의 이벤트**라는 점에서 지금까지의 두 랩과 구분되는 새로운 유형입니다.

이 랩의 mbox/센서 관련 설계 패턴(ISR 세이프티 규칙, 베이스라인 캘리브레이션, send-on-change)은 이 프로젝트와는 별도로 진행되어 **이미 실기로 검증이 끝난** M4↔M55 IPC 커리큘럼(다른 git 저장소, 동일 보드/SoC 대상)의 확립된 패턴을 그대로 가져온 것입니다. **실기 확인 결과 (2026-09-21)**: 이 저장소에서도 별도 수정 없이 그대로 동작했습니다 — devicetree 노드명(`mbox_consumer`/`ipc0`/`mc3479`)과 Kconfig 심볼(`CONFIG_MC3419`/`CONFIG_MBOX`) 모두 이 SDK와 정확히 일치했고, 보드를 흔들면 M4→M55 이벤트가 끊김 없이 전달됨을 확인했습니다.

## 하드웨어 구성

- M55 콘솔과 M4 콘솔을 모두 열어둡니다(230400bps, 8N1 또는 온보드 브릿지 115200bps — 이전 랩과 동일).
- 온보드 가속도계(`mc3479`)가 M4의 I2C1에 물려 있어야 합니다. 별도의 외부 배선은 필요 없습니다.
- 보드를 평평한 곳에 가만히 둔 채로 부팅해야 베이스라인 캘리브레이션이 올바르게 잡힙니다(캘리브레이션 중 보드를 움직이면 그 움직임이 베이스라인에 그대로 섞여 들어갑니다 — 아래 "코드 구성" 참고).

## Devicetree 오버레이

Lab 01에서 다뤘던 I2C1 버스 공유 문제가 이번에는 **반대 방향**으로 다시 등장합니다. Lab 01은 M55가 I2C1을 쓰고 M4가 양보했다면, 이번 Lab 03은 M4가 가속도계를 읽기 위해 I2C1을 써야 하므로 M55가 양보합니다.

`lab/boards/sr100_rdk_sr100_m55.overlay` (M55 — I2C1과 그 자식 노드를 모두 비활성화):
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

`lab/remote/boards/sr100_rdk_sr100_m4.overlay` (M4 — `mc3479`만 명시적으로 활성화):
```dts
&mc3479 {
	status = "okay";
};

&ipc0 {
	shared-memory-size = <0x400>;
};
```

Lab 01의 트러블슈팅 문서에서 확인했듯, **부모 버스(`i2c1`)만 비활성화하는 것으로는 부족합니다** — 자기 자신의 `status`를 명시하지 않은 자식 노드는 base devicetree의 기본값(`okay`)에 그대로 남아 비활성화된 부모를 계속 참조하다가 컴파일 에러(`__device_dts_ord_23 undeclared` 류)를 냅니다. 그래서 `gpio_exp0`, `ov02c10`도 함께 명시적으로 비활성화했습니다.

`i2c1` 자체는 base devicetree에서 이미 `okay`가 기본값이므로 M4 쪽 오버레이에서 별도로 켤 필요는 없습니다(Lab 01의 M55 쪽 오버레이와 정반대 방향의 동일한 패턴). 반면 `mc3479`는 base devicetree에서 `status = "disabled"`로 시작하므로 명시적으로 켜야 합니다.

`ipc0`의 `shared-memory-size`는 M55/M4 양쪽 오버레이에서 정확히 같은 값이어야 합니다.

**실기 확인 결과 (2026-09-21)**: `gpio_exp0`, `ov02c10`, `i2c1`, `ipc0`, `mc3479` 노드 라벨과 `mbox_consumer` 경로 모두 별도 수정 없이 그대로 빌드/동작했습니다 — 별도 IPC 커리큘럼 저장소에서 가져온 이름이 이 저장소의 devicetree와 100% 일치함을 확인했습니다.

## 코드 구성

### 공용 헤더 (`lab/include/ipc_common.h`, `lab/remote/include/ipc_common.h`)

M4 → M55로만 흐르는 단일 메시지 타입이라 별도의 type/command 태그 없이 구조체 하나로 충분합니다.

```c
enum motion_state {
	MOTION_STATE_STILL = 0,
	MOTION_STATE_ACTIVE = 1,
};

struct ipc_motion_event {
	uint32_t seq;          /* 이벤트 일련번호, 1부터 시작 */
	uint8_t state;         /* enum motion_state: 방금 진입한 상태 */
	int32_t deviation_mg;  /* 상태가 바뀐 순간의 |dx|+|dy|+|dz| (베이스라인 대비, milli-g) */
};
```

이전 IPC 커리큘럼의 관례를 그대로 따라, 이 헤더를 M55/M4 양쪽 `include/` 폴더에 각각 복사해 두었습니다.

### M4 (`lab/remote/src/main.c`) — 가속도계 모니터

1. **베이스라인 캘리브레이션**: 부팅 직후 약 1초(`ACCEL_CALIB_SAMPLES = 10`회, 100ms 간격) 동안 샘플을 평균 내어 중력/장착 오프셋을 한 번만 스냅샷으로 저장합니다. 이후 모든 샘플은 0이 아니라 **이 베이스라인 대비 편차**로 임계값과 비교합니다. 캘리브레이션 중 보드가 움직이면 그 움직임이 베이스라인에 그대로 섞여 들어간다는 점에 주의합니다 — 적응형 필터가 아니라 고정 스냅샷입니다.
2. **센서 초기화 순서**: `mc3479`는 초기화 직후 저전력 대기 상태에 머물러 있어서, `sensor_attr_set()`으로 출력 데이터 레이트(ODR)와 풀스케일 레인지를 **둘 다** 명시적으로 설정하기 전까지는 에러 없이 조용히 항상 0만 반환합니다. 둘 중 하나만 빠뜨려도 증상이 똑같이 "0만 나옴"이라 원인 파악이 까다로우므로, 코드에서 두 호출을 나란히 붙여 두었습니다.
3. **send-on-change**: 매 샘플마다 보내는 것이 아니라, 상태(`MOTION_STATE_STILL`/`MOTION_STATE_ACTIVE`)가 실제로 바뀐 순간에만 `mbox_send_dt()`를 호출합니다. "임계값 초과 시마다 보내기"로 순진하게 설계하면 보드가 임계값 근처에 걸쳐 있을 때 매 폴링마다 알림이 쏟아지는데, 이를 피하기 위한 설계입니다.
4. `mbox_send_dt()`는 `main()` 스레드 자신의 컨텍스트(폴링 루프)에서만 호출됩니다. 이 랩에서 M4는 rx 콜백을 전혀 열지 않으므로(M55가 응답을 보내지 않는 단방향 설계), ISR 컨텍스트에서 실수로 `mbox_send_dt()`를 호출할 여지 자체가 없습니다.

### M55 (`lab/src/main.c`) — 리스너

1. `mbox_register_callback_dt()` + `mbox_set_enabled_dt()`로 rx 채널만 엽니다. 이 랩은 M55 → M4 응답이 전혀 없는 일방향 "wake" 채널이라, M55 쪽에는 `mbox_send_dt()` 호출이 아예 없습니다.
2. **rx 콜백은 ISR 컨텍스트에서 실행됩니다** — 이는 이 프로젝트와 별도로 이미 실기 검증된 M4↔M55 IPC 커리큘럼에서 확립된 규칙입니다. 그래서 콜백 안에서는 메시지를 `k_msgq_put(..., K_NO_WAIT)`으로 큐에 복사해 넣고 즉시 리턴할 뿐, 로그 출력을 포함한 실제 처리는 전혀 하지 않습니다.
3. 실제 로그 출력 등 처리는 `main()`의 while 루프 안, 별도 스레드 컨텍스트에서 `k_msgq_get(&evt_msgq, &evt, K_FOREVER)`로 큐를 받아온 뒤에 이루어집니다.
4. `K_FOREVER`로 무기한 블로킹하는 이유: 이 랩에서는 "메시지가 없다"는 것 자체가 아무 의미를 갖지 않습니다(단순히 아직 움직임이 없었다는 뜻). 그래서 하트비트 감시처럼 "일정 시간 안에 메시지가 안 오면 그것 자체가 이상 신호"인 경우와 달리, 유한한 타임아웃을 둘 이유가 없습니다. 이 상태에서 다른 실행 가능한 스레드가 없으면 Zephyr의 idle 스레드가 자동으로 WFI에 들어갑니다.
5. **주의(혼동하기 쉬운 지점)**: `STILL`/`ACTIVE`는 M4 가속도계가 보는 **모션 상태**이고, WFI/idle은 M55의 **CPU 전력 상태**로 서로 다른 축입니다. "이벤트가 `STILL`일 때만 idle로 간다"는 뜻이 아니라, `ACTIVE`든 `STILL`이든 **이벤트를 하나 처리하고 나면 항상** 루프 맨 위의 `k_msgq_get(K_FOREVER)`로 돌아가고, 그 블로킹 지점에서 다음 이벤트가 올 때까지 WFI가 유지됩니다. 보드를 가만히 두면 이벤트 자체가 안 들어오니 "블로킹 상태 = idle"로 오래 보이는 것뿐입니다.

## prj.conf

`lab/prj.conf` (M55) — 이 랩에서 M55는 mbox rx만 쓰므로 최소 구성입니다:
```
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SERIAL=y

CONFIG_MBOX=y
```

`lab/remote/prj.conf` (M4) — I2C1, 센서 서브시스템, `mc3479` 드라이버, mbox가 추가되었습니다:
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

**실기 확인 결과 (2026-09-21)**: `CONFIG_MC3419`(드라이버 심볼명, devicetree 노드 라벨은 `mc3479`)와 `CONFIG_MBOX` 모두 이 저장소에서 그대로 빌드에 성공했습니다.

## 빌드 방법

이전 랩들과 동일한 방식입니다(M4를 먼저 빌드한 뒤 M55에 패키징).

```bash
# 1) M4를 먼저 빌드합니다.
west build -p always -b sr100_rdk/sr100/m4 -d m4 03_ipc_sensor_wake/lab/remote

# 2) M55를 빌드하면서 M4 이미지를 함께 패키징합니다.
west build -p always -b sr100_rdk/sr100/m55 -d m55 03_ipc_sensor_wake/lab \
    -- -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

## 실행 및 결과 확인

**실기 확인 완료 (2026-09-21)**: 별도 수정 없이 첫 빌드/플래시부터 정상 동작했습니다.

1. M4와 M55 양쪽 콘솔을 모두 열고 플래시합니다. 보드는 평평한 곳에 가만히 둡니다.
2. M55 콘솔은 부팅 직후 대기 상태로 들어가 조용합니다.
   ```
   === Lab 03 (M55): waiting for M4 motion-state notifications ===
   [M55] idle, waiting for events (CPU goes to WFI between them)...
   ```
3. 보드를 손으로 들어 흔들었다 내려놓기를 반복하면, M55 콘솔에 다음과 같이 상태 전환마다 이벤트가 하나씩 찍힙니다(실제 관찰된 로그):
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
   흔드는 동안 매 폴링(100ms)마다 알림이 오는 것이 아니라, `seq` 번호가 상태 전환 횟수만큼만(흔든 시간과 무관하게) 올라가는 것을 확인했습니다 — send-on-change 설계가 의도대로 동작합니다.
4. `ACCEL_THRESHOLD_MILLI_G = 2000` 기본값 그대로, 가만히 둔 상태(STILL)에서는 편차가 임계값 아래(약 1000 milli-g 이하)로, 흔들었을 때(ACTIVE)는 임계값을 넉넉히 넘는 값(약 2000 후반대)으로 나뉘어 재조정 없이도 오탐 없이 잘 구분됨을 확인했습니다.
5. **확인 도구로 추가했던 디버그 로그**: 이벤트가 `ACTIVE`/`STILL` 어느 쪽이든 처리된 직후 매번 `k_msgq_get(K_FOREVER)`로 되돌아간다는 것(= 그 지점에서 WFI가 다시 가능해진다는 것)을 눈으로 확인하기 위해 일시적으로 `"[M55] back to waiting..."` 로그를 추가해 검증했습니다. 검증 후 이 줄은 코드 정리 과정에서 제거했습니다(최종 코드에는 없음) — 정말 CPU가 WFI로 돌아갔는지(전류 실측 등)까지 확인하려면 Lab 01과 같은 멀티미터 측정이 별도로 필요합니다.

## 확인 필요 사항 (TBD)

- ~~이 코드 전체가 이 저장소에서 아직 실기 테스트를 거치지 않음~~ — **확인 완료(2026-09-21)**: devicetree 노드명/Kconfig 심볼 모두 별도 수정 없이 그대로 빌드/동작.
- ~~`ACCEL_THRESHOLD_MILLI_G = 2000`이 이 보드에서 재조정이 필요할 수 있음~~ — **확인 완료**: 기본값 그대로 STILL/ACTIVE가 오탐 없이 잘 구분됨.
- `sensor_attr_set(..., SENSOR_ATTR_FULL_SCALE, &range)`에서 `range.val1 = 0`("드라이버 기본/최소 레인지")이 정확히 어떤 물리적 레인지(±2g/±4g 등)에 대응하는지는 여전히 확인하지 않았습니다 — 기능적으로는 문제없이 동작했으므로 이번 커리큘럼 범위에서는 더 파고들지 않기로 합니다.
- mbox 콜백이 정말 ISR 컨텍스트에서 실행되는지, `mbox_send_dt()`가 이 보드에서 블로킹될 가능성(별도 IPC 커리큘럼 Lab 07에서 확인된 이슈)이 이 랩에서도 재현되는지는 이번 테스트로는 직접 확인되지 않았습니다(문제가 드러날 만한 부하가 아니었음) — 설계상 안전한 패턴을 그대로 따랐다는 점에 의존하고 있습니다.

## 다음 랩 예고

Lab 03까지 Lab 00~02(소스 리뷰 → WFI → watchdog 리셋)와 서로 다른 "깨어남" 세 가지를 모두 실습하고, 전부 실기 확인까지 마쳤습니다. 다음 Lab 04는 이 커리큘럼에서 가장 탐색적인 랩으로, `RESET_LOW_POWER_WAKE`와 보드의 물리 Wake-up 버튼(`SW7`으로 추정) 경로를 실기로 찾아보는 실험입니다. 성공을 보장할 수 없는 랩이라 찾은 만큼만 정직하게 문서화할 예정입니다.

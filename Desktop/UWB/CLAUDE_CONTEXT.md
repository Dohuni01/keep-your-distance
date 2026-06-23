# CLAUDE_CONTEXT.md
_이 파일은 미래의 Claude 인스턴스가 이 프로젝트를 빠르게 파악하고
올바른 방향으로 작업할 수 있도록 작성된 컨텍스트 문서다._

---

## 프로젝트 한 줄 요약

공사현장에서 UWB(DW3000)로 작업자와 장비 간 거리를 측정해
3m 이내 접근 시 GPIO26으로 장비를 자동 정지시키는 안전 시스템.

---

## 절대 어기면 안 되는 원칙

### 1. GPIO26 안전 제어는 네트워크 독립

릴레이 제어 코드(`setRelay`, `processTagDistance`, `processNoRange`)는
WiFi, MQTT, 서버 상태를 참조하거나 대기하면 안 된다.
현재 구조: Core 1의 `taskUWB`가 릴레이를 단독 제어.
Core 0의 `taskNetwork`는 데이터를 읽기만 한다.

이 원칙을 깨는 코드는 어떤 이유로도 작성하지 않는다.

### 2. 공유 데이터는 단방향

`g_tagReports`, `g_relayState`, `g_reportTimestamp`는
Core 1(UWB)이 쓰고 Core 0(Network)이 읽는다.
Core 0이 이 변수들에 값을 쓰는 코드를 추가하면 안 된다.
접근 시 반드시 `portENTER_CRITICAL(&g_reportMux)` 사용.

### 3. GPS는 지도 표시 전용

GPS 데이터는 Anchor의 지도 위치 표시에만 쓴다.
안전 거리 측정은 UWB만 담당한다.
GPS 신호 유무가 릴레이 동작에 영향을 주면 안 된다.

---

## 핵심 설계 결정 및 이유

### FreeRTOS 이중 코어 분리
WiFi 재연결이나 MQTT 처리가 수백ms 블로킹될 수 있다.
`loop()`에 네트워크 코드를 섞으면 UWB 폴링이 멈춰 안전 공백이 생긴다.
이를 막기 위해 Core 1(UWB)과 Core 0(Network)을 완전히 분리했다.

### 라운드로빈 폴링
DW3000은 1:1 TWR 방식이라 동시에 여러 태그와 통신할 수 없다.
Anchor가 등록된 태그를 순서대로 하나씩 호출한다.
태그 4개 기준 전체 주기 = 600ms (150ms × 4).

### 히스테리시스 (3.00m 진입 / 3.20m 이탈)
20cm 간격으로 릴레이 채터링(빠른 ON/OFF 반복)을 방지한다.
또한 이탈 판정은 3회 연속이어야 한다 (CONFIRM_OFF_COUNT=3).

### `noRangeTick`에서 `outsideCount` 리셋 금지
신호가 끊겼을 때 outsideCount를 리셋하면,
신호 단절 중에 이탈 카운트가 누적되지 않아 릴레이가 영원히 ON으로 남는다.
`processNoRange`는 `outsideCount`를 건드리지 않는다.

### WDT를 `taskUWB` 내부에서 등록
`setup()`에서 `esp_task_wdt_add(NULL)`을 호출하면
`loop()`가 `vTaskDelete`로 삭제된 후 해당 태스크가 WDT를 리셋하지 않아
15초 후 재부팅된다.
WDT 등록은 `taskUWB` 첫 줄에서 수행한다.

### MQTT retain=true (relay, status)
대시보드가 뒤늦게 접속해도 마지막 릴레이 상태와 온라인 여부를
브로커로부터 즉시 받을 수 있다.
거리 데이터(range)는 실시간성이 중요하므로 retain=false.

### LWT (Last Will Testament)
Anchor가 비정상 종료(전원 차단, crash)되면
브로커가 자동으로 `uwb/anchor/{id}/status`에 `{"online":false}`를 발행한다.
대시보드가 Anchor 오프라인을 능동적으로 감지할 수 있다.

---

## 프레임 구조 (반드시 숙지)

```
인덱스:  0     1     2     3     4     5          6        7        8    9
값:    0x41  0x88  SEQ  0xCA  0xDE  ANCHOR_ID  TAG_ID  MSG_TYPE  0x00 0x00

MSG_TYPE: 0xE0 = poll (Anchor → Tag)
          0xE1 = response (Tag → Anchor)

response 추가 필드:
  [10-13] poll_rx_ts  (uint32, 4바이트)
  [14-17] resp_tx_ts  (uint32, 4바이트)
  총 18바이트
```

SEQ(인덱스 2)는 비교 전 0으로 마스킹.
Anchor는 ANCHOR_ID + TAG_ID + MSG_TYPE 세 필드를 동시에 검증한다.
Tag는 MSG_TYPE + TAG_ID 두 필드만 검증 (어느 Anchor든 수락).

---

## 파일별 역할

| 파일 | 역할 |
|---|---|
| `Anchor/ancher_v1/ancher_v1.ino` | Anchor 펌웨어. Phase 2-B까지 구현됨 |
| `Tag/tag_v1/tag_v1.ino` | Tag 펌웨어. Phase 2-A 상태 |
| `PROJECT_STATUS.md` | 현재 구현 상태, 알려진 이슈, 플래시 방법 |
| `ROADMAP.md` | 전체 단계별 계획 및 기술 스택 |
| `CLAUDE.md` | Arduino 빌드/플래시 명령, 핀 배열 등 |

---

## 현재 미구현 (다음 작업 대상)

우선순위 순서:
1. **Phase 2-C**: GPS 모듈 연결 — NEO-6M, UART(GPIO16/17), TinyGPS++ 라이브러리
2. **Phase 3**: Node.js 백엔드 + Mosquitto 브로커 설치 (VPS)
3. **Phase 4**: Leaflet.js 웹 대시보드
4. **Phase 5**: Tag 절전, OTA, 2단계 경보 구역

---

## 주의사항

- `PubSubClient` 기본 최대 메시지 크기 = 256바이트. 현재 페이로드는 이 안에 들어옴
- `taskNetwork` 스택 = 8192 (WiFi 내부 TLS 스택 고려). 줄이지 말 것
- 태그 등록은 현재 컴파일 타임 고정 (`TAG_IDS[]`). 런타임 동적 등록은 Phase 5 이후 검토
- 안테나 딜레이(16385)는 미교정 기본값. 실측 교정 전까지 거리 오차 ±10~30cm 예상
- `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_BROKER`는 `ancher_v1.ino` 상단 define에서 수정

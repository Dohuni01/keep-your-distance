# PROJECT_STATUS.md
_Last updated: 2026-06-23 (기술 리뷰 반영)_

---

## 프로젝트 개요

공사현장 UWB 기반 안전거리 감지 시스템.
굴착기 등 위험 장비에 Anchor를 부착하고, 작업자가 Tag를 착용.
작업자가 3m 이내 접근 시 GPIO26으로 신호 출력 → 장비 정지.

---

## 하드웨어

| 항목 | 내용 |
|---|---|
| 보드 | Makerfabs ESP32 UWB DW3000 × 5 |
| 현재 구성 | Anchor 1대 + Tag 4대 |
| 릴레이 핀 | GPIO26 (Anchor 전용) |
| UWB 핀 | RST=27, IRQ=34, SS=4 |
| SPI | 16MHz, MSBFIRST, SPI_MODE0 |
| 안테나 딜레이 | TX/RX 각 16385 (**미교정 상태 — Phase A 교정 필요**) |
| ⚠️ 보드 변종 | GPS(GPIO16/17) 연결 전 WROOM/WROVER 확인 필요 (WROVER는 PSRAM 충돌) |

---

## 현재 구현 상태

### ✅ Phase 1 — 기반 안정화 (완료)

- `Sleep()` → `delay()` 수정
- 시리얼 메시지 오류 수정 (`No range x5` → `x20`)
- 미사용 `insideCount`, `CONFIRM_ON_COUNT` 제거
- 워치독 타이머 15초 (Anchor) / 10초 (Tag)
- 거리 이동 평균 필터 5회 (유효값만 누적) ← **Phase A에서 비대칭 median으로 교체 예정**
- 부팅 시 Relay OFF 보장

### ✅ Phase 2-A — 다중 태그 UWB 프로토콜 (완료)

- 프레임에 ANCHOR_ID + TAG_ID 필드 추가
- Anchor: 라운드로빈 순차 폴링 (Tag#1→2→3→4→반복)
- Tag: 자신의 MY_TAG_ID에 해당하는 poll에만 응답
- TagState 구조체로 태그별 독립 위험 상태 관리
- 릴레이: 태그 하나라도 위험 → ON / 전부 안전 → OFF
- Tag가 senderAnchorId를 echo → 향후 다중 앵커 대응 가능

### ✅ 안전 독립성 설계 (완료, 핵심 강점)

- FreeRTOS 이중 코어 분리
  - Core 1: `taskUWB` (우선순위 2) — UWB + GPIO26 전담
  - Core 0: `taskNetwork` (우선순위 1) — WiFi + MQTT 전담
- portMUX로 공유 데이터 보호 (단방향: Core1 쓰기 / Core0 읽기)
- 네트워크 장애가 UWB 안전 루프에 영향 없음을 구조적으로 보장
- WDT를 `taskUWB` 내부에서 등록 (setup 태스크 삭제 후 WDT 오동작 방지)

### ✅ Phase 2-B — WiFi + MQTT (완료)

- `taskNetwork`에 WiFi 연결 + 자동 재연결
- PubSubClient 기반 MQTT publish
- LWT(Last Will Testament) — 비정상 종료 시 브로커가 offline 발행
- 릴레이 변경 즉시 publish (retain=true) / 거리 1초 / status 30초 (retain=true)

### ✅ 대시보드 Mock UI (완료, 실데이터 미연동)

- Leaflet 다크 지도, Anchor 5 + Tag 20 마커
- 좌측 Anchor/Tag 목록 + 통계, 우측 위험 목록 + 이벤트 로그
- `DataSource` 추상화 — `USE_MOCK` 플래그로 실연동 전환 준비됨
- ⚠️ 태그 지도 좌표는 앵커 주변 **가짜 오프셋** (방위 데이터 없음)

---

## 🔴 기술 리뷰 발견 — 우선 수정 대상 (Critical)

_2026-06-23 시니어 임베디드/풀스택 리뷰 결과. 상세는 ROADMAP.md Phase A~D 참조._

| ID | 발견 | 위치 | 영향 | 단계 | 상태 |
|---|---|---|---|---|---|
| **C1** | 5-샘플 이동평균 = 약 3초 탐지 지연 | `ancher_v1.ino` | 1.5m/s 접근 시 ~4.5m 좁혀진 뒤 탐지. **정지가 늦음** | A | ✅ 코드 수정 (비대칭 median, 진입 3 / 이탈 5) |
| **C2** | 태그 RX 타임아웃 없는 무한 스핀 | `tag_v1.ino` | 앵커 부재 시 10초마다 리부트(배터리·가용성 손실) | A | ✅ 코드 수정 (`dwt_setrxtimeout` + re-arm) |
| **C3** | 앵커 간 RF 조율 없음 | 프로토콜 전반 (단일 채널 5) | 앵커 5대 동시 폴링 시 충돌 → 거짓 no-range | B | N/A (앵커 1대 고정 — 5보드 = 1앵커+4태그) |
| **C4** | 타임스탬프가 `millis()`(가동시간) | 모든 MQTT payload | 앵커 간 상관·리부트 후 추적 불가 → 컴플라이언스 로그 무력 | C | ⬜ 미착수 |
| **C5** | 안테나 딜레이 미교정 (±10~30cm) | `TX/RX_ANT_DLY 16385` | 3m 임계값에서 절반은 **늦게** 정지 | A | 🟡 2점 교정 SOP 주석화, 실측 교정은 하드웨어 필요 |

> **Phase A 코드 적용 완료 (2026-06-23)** — C1·C2는 펌웨어 반영, C5는 교정 SOP 주석 추가.
> **Phase B 완료 (2026-06-23)** — C3 해당 없음(1앵커 확정). 적응형 dead-tag 폴링 구현:
> `absentCount`/`pollSkip`으로 10회 연속 no-range 태그는 1-in-5 슬로 폴링 전환.
> 위험 상태 태그는 슬로 폴링 예외 적용 — 안전 보장.
> 모두 **하드웨어 실측 검증 대기** (`arduino-cli` 미설치로 본 환경 컴파일 미수행).
> 추가: `taskNetwork`는 의도적으로 panic WDT에 등록하지 않음(안전 독립성) — 코드 주석화.

### 추가 발견 (High/Medium)

- **멀티패스 취약**: 평균 필터가 스파이크에 약함 → median 권장 (A)
- **보안 레인징 부재**: STS OFF, 프레임 무인증 → 거리 단축 공격 가능 (A 검토)
- **`taskNetwork` WDT 미감시**: 네트워크 태스크 hang 미탐지 (A)
- **PubSubClient QoS0 only**: relay 전이가 fire-and-forget, 브로커 blip 시 손실 (C/D — staleness로 보완)
- **MQTT 평문(1883)**: 자격증명·위치 노출 → TLS 필요 (C)
- **손수 짠 JSON**: NaN/inf 시 파싱 깨짐 → ArduinoJson (C)
- **clientId 충돌**: 동일 ANCHOR_ID 시 브로커 강퇴 → MAC 추가 (C)
- **태그 정적 등록**: `TAG_IDS[]` 컴파일타임 → 동적 레지스트리 (C)
- **백엔드 단일 장애점 + store-and-forward 부재**: 다운 중 이벤트 로그 공백 (C/D)
- **이벤트 권위 모호**: 앵커가 판정해야 함, 대시보드 재계산 금지 (D)
- **대시보드 staleness 미표시**: 멈춘 화면이 "전부 안전"처럼 보임 (E)
- **태그 지도 위치 허구**: 방위 데이터 없음 → 거리 링/목록으로 정직화 (F)

---

## 파일 구조

```
UWB/
├── Anchor/ancher_v1/ancher_v1.ino   Anchor 펌웨어 (Phase A·B 반영)
├── Tag/tag_v1/tag_v1.ino            Tag 펌웨어 (Phase A C2 반영)
├── dashboard/                       Mock 대시보드 (index.html, style.css, script.js)
├── tests/
│   └── anchor_collision_test/       테스트 A — 다중 앵커 충돌 측정 도구
│       ├── anchor_collision_test.ino   (프로덕션 무수정 별도 스케치)
│       └── README.md                   실행 절차·판정 기준
├── PROJECT_STATUS.md                이 파일
├── ROADMAP.md                       전체 로드맵 (Phase A~G + Scale-out)
└── CLAUDE_CONTEXT.md                Claude 인스턴스용 컨텍스트
```

---

## 빌드/플래시 환경 (2026-06-23 확인)

| 항목 | 상태 |
|---|---|
| `arduino-cli` (독립) | ❌ 미설치 |
| IDE 번들 `arduino-cli` | ✅ `C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe` |
| esp32 코어 | ✅ 3.3.7 (Latest 3.3.10) |
| Dw3000 라이브러리 | ✅ `Documents\Arduino\libraries\Dw3000` |
| PubSubClient | ❌ **미설치** — 프로덕션 앵커 컴파일에 필요 (테스트 A는 불필요) |
| 테스트 A 스케치 컴파일 | ✅ 통과 (FQBN `esp32:esp32:esp32`, flash 22% / RAM 6%) |

**프로덕션 플래시 전 처리 필요 (테스트 A 이후):**
1. `PubSubClient by Nick O'Leary` 설치 (없으면 앵커 컴파일 실패)
2. **esp32 코어 3.x WDT 시그니처** — `esp_task_wdt_init(WDT_TIMEOUT_S, true)`의
   타임아웃 단위가 코어 3.x에서 초→밀리초로 바뀌어 의도와 다르게 동작할 소지. 실측 확인 필요.

컴파일 예:
```powershell
$cli = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$lib = "$env:USERPROFILE\Documents\Arduino\libraries"
& $cli compile --fqbn esp32:esp32:esp32 --libraries $lib "C:\Users\bbk\Desktop\UWB\tests\anchor_collision_test"
```

---

## 🟡 Scale-out (100대 확장) — 2026-06-23 확장성 리뷰

실제 목표는 현장 규모(앵커 10~50 / 태그 50~100). 리뷰 결론·결정:

- **현재 구조는 "단일 셀 근접 PoC"로는 우수하나 사이트 전역 RTLS로 확장되는 구조가 아님.**
- 1순위 병목 = **UWB 단일 채널 충돌**(MAC 없음). WiFi/MQTT는 2차.
- 핵심 재정의: **RTLS가 아니라 근접(proximity)**. 100명 추적 불필요.
- 공존 순서: **공간재사용 → 2채널(ch5/9) → (측정이 강요할 때만) TDMA**.
- 태그: **하이브리드**(blink=발견, 앵커 능동 폴링=안전). 순수 tag-initiated는 거짓안전 위험.
- 최대 위험: **양성감지 의존 + NLOS 금속 false-safe** — 차폐된 작업자를 "부재"로 오분류.
- **측정 우선**: 테스트 A(충돌)/B(NLOS)/D(재사용거리)/E(채널분리)로 물리 한계부터 측정.
  - 테스트 A 도구 완료(`tests/anchor_collision_test/`), 측정 대기.

상세 단계는 `ROADMAP.md`의 **Scale-out 전략** 섹션 참조.

---

## MQTT 토픽 구조 (현재)

```
uwb/anchor/{id}/tag/{tag_id}/range  — 거리 데이터, 1초 주기, retain=false
uwb/anchor/{id}/relay               — 릴레이 상태, 변경 즉시, retain=true
uwb/anchor/{id}/status              — 헬스체크, 30초 주기, retain=true
```

페이로드 예시 (⚠️ `ts`는 현재 epoch가 아닌 `millis()` — Phase C에서 교정):
```json
// range
{"raw":2.85,"filt":2.90,"danger":false,"ts":123456}
// relay
{"state":true,"ts":123456}
// status
{"online":true,"anchor_id":1,"tags":4,"uptime":360,"ts":123456}
```

---

## 폴링 타이밍

| 항목 | 값 |
|---|---|
| 태그 1개당 폴링 간격 | 150ms |
| 전체 사이클 (태그 4개, 전원 정상) | 600ms |
| 각 태그 측정 주기 | ~1.67Hz |
| RX 타임아웃 (Anchor) | 3ms |
| 비대칭 median 진입 지연 | median(3) = 약 **450ms** (C1 수정, 기존 3초→단축) |
| 비대칭 median 이탈 지연 | median(5)+3회연속 = 약 3초 (의도된 디바운스) |
| **슬로 폴링 전환 기준 (Phase B)** | 10회 연속 no-range → 1-in-5 (해당 태그 ~3s 주기) |
| **슬로 폴링 해제 조건 (Phase B)** | 성공 수신 즉시 전체 속도 복귀 |
| 위험 중 태그 슬로 폴링 예외 | 없음 (안전 보장 — danger 태그는 슬로 적용 금지) |

---

## 다음 작업 (우선순위)

1. ✅ **Phase A — 안전 정확성**: C1(비대칭 median), C2(태그 RX 타임아웃) — 코드 완료, 실측 교정 대기(C5)
2. ✅ **Phase B — 적응형 폴링**: C3 N/A(1앵커 고정). dead-tag 슬로 폴링 구현 완료
3. **Phase C — 펌웨어 신뢰성**: NTP, ArduinoJson, clientId, 동적 태그, 이벤트 버퍼, TLS
4. **Phase D — 백엔드 + DB**: 브로커 하드닝, Postgres+Influx, 인증, staleness
5. **Phase E — 대시보드 실연동**: `USE_MOCK=false`, staleness UI, STOP/RUN 우선
6. **Phase F — 측위/GPS**, **Phase G — 강화/법규**

---

## 플래시 방법 (5개 보드)

| 보드 | 펌웨어 | 변경 항목 |
|---|---|---|
| #1 | Anchor | `ANCHOR_ID 1`, `TAG_IDS[] = {2,3,4,5}` |
| #2 | Tag | `MY_TAG_ID 2` |
| #3 | Tag | `MY_TAG_ID 3` |
| #4 | Tag | `MY_TAG_ID 4` |
| #5 | Tag | `MY_TAG_ID 5` |

필수 라이브러리: `PubSubClient by Nick O'Leary` (Arduino Library Manager)

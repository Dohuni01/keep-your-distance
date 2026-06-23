# ROADMAP.md
_공사현장 UWB 안전 시스템 — 단계별 구현 계획 (2026-06-23 기술 리뷰 반영 개정판)_

---

## 목표

실제 공사현장에서 즉시 투입 가능한 UWB 기반 작업자 안전거리 감지 시스템.
- 굴착기 등 위험 장비에 Anchor 부착
- 작업자가 Tag 착용
- 3m 이내 접근 시 장비 자동 정지 (GPIO26 릴레이)
- 현장 감독관이 웹 브라우저로 실시간 모니터링

---

## 개정 배경 (왜 순서를 바꿨나)

기존 로드맵은 **GPS → 백엔드 → 대시보드 → 강화** 순서였다.
그러나 기술 리뷰 결과, 이 순서는 "3m 정지를 신뢰할 수 있게 만드는 일"을 뒤로 미룬다.

핵심 원칙: **안전 정확성(ranging correctness)이 모니터링보다 먼저다.**
- 필터 지연·태그 리부트·안테나 미교정은 "장비가 올바른 거리/시점에 멈추는가"를 결정한다.
- GPS·백엔드는 모니터링 편의 기능이며 안전 정지의 신뢰도와 무관하다.

따라서 **안전 정확성 → 다중 앵커 RF → 데이터 신뢰성 → 백엔드/DB → 대시보드 실연동 → 측위/GPS → 강화** 순으로 재배치한다.

---

## 전체 단계 현황

```
Phase 1    기반 안정화                    ✅ 완료
Phase 2-A  다중 태그 UWB 프로토콜         ✅ 완료
Phase 2-B  Anchor WiFi + MQTT            ✅ 완료
대시보드   Mock 기반 UI (Anchor+Tag 표시) ✅ 완료 (실데이터 미연동)
────────────────────────────────────────────────────
Phase A    안전 정확성          🔴 최우선  ⬜ 미구현
Phase B    다중 앵커 RF 공존    🔴 최우선  ⬜ 미구현
Phase C    필드급 펌웨어 신뢰성            ⬜ 미구현
Phase D    백엔드 + DB                    ⬜ 미구현
Phase E    대시보드 실연동                ⬜ 미구현
Phase F    측위 & GPS                     ⬜ 미구현
Phase G    현장 강화 & 법규 준수          ⬜ 미구현
```

🔴 = 안전 정지의 신뢰도를 직접 결정하는 단계. 다른 무엇보다 먼저.

---

## 상세 계획

---

### ✅ Phase 1 — 기반 안정화 (완료)

- 버그 수정 (`Sleep()` → `delay()`, 시리얼 메시지 오류)
- 미사용 변수 제거 (`insideCount`, `CONFIRM_ON_COUNT`)
- 워치독 타이머 추가
- 거리 이동 평균 필터 (5회) ← **Phase A에서 재검토 대상**
- 부팅 시 Relay OFF 보장

### ✅ Phase 2-A — 다중 태그 UWB 프로토콜 (완료)

- 프레임에 ANCHOR_ID + TAG_ID 추가
- Anchor 라운드로빈 폴링
- Tag 자신의 ID에만 응답
- 태그별 독립 위험 상태 관리

### ✅ Phase 2-B — Anchor WiFi + MQTT (완료)

- FreeRTOS 이중 코어 분리 (UWB/네트워크 완전 독립)
- WiFi 연결 + 자동 재연결
- PubSubClient MQTT publish (LWT, retain)

### ✅ 대시보드 Mock UI (완료, 실데이터 미연동)

- Leaflet 다크 지도, Anchor 5 + Tag 20 마커
- 좌측 Tag 목록, 우측 위험 목록 + 이벤트 로그
- `DataSource` 추상화 (`USE_MOCK` 플래그 하나로 실연동 전환)
- ⚠️ 태그 지도 좌표는 **앵커 주변 가짜 오프셋** (방위 데이터 없음 — Phase F에서 정직하게 교정)

---

## 🔴 Phase A — 안전 정확성 (최우선)

**목적**: 3m 정지가 "올바른 거리에서, 올바른 시점에" 작동하도록 만든다.
현재 가장 큰 안전 리스크들이 여기 모여 있다.

**작업 내용**

| ID | 문제 | 작업 |
|---|---|---|
| C1 | 5-샘플 이동평균 = 약 3초 탐지 지연 | **비대칭 필터**: 진입은 raw/median(3)로 즉시, 이탈은 median(5)로 신중하게 |
| C1' | 평균 필터가 멀티패스 스파이크에 취약 | 평균 → **중앙값(median) 필터**로 교체 |
| C2 | 태그가 RX 타임아웃 없이 무한 스핀 → 앵커 부재 시 10초마다 리부트 | 태그에 `dwt_setrxtimeout` + **re-arm 루프**, 매 반복 WDT reset |
| C5 | 안테나 딜레이 미교정 (±10~30cm) | **2점 교정 SOP** 수립 (known distance), 앵커별 캘리브레이션 값 저장 |
| — | 보안 레인징 부재 (STS OFF) | DW3000 **STS** 적용 검토 (거리 단축 공격 방어) |
| — | `taskNetwork` WDT 미감시 | 네트워크 태스크 WDT 구독 또는 하트비트 추가 |

**검증 기준**
- 1.5 m/s로 접근하는 태그가 3.0m 통과 후 0.6초(1 사이클) 이내 릴레이 ON
- 멀티패스 스파이크 1회가 위험 판정을 지연시키지 않음
- 앵커 전원 차단 시 태그가 리부트하지 않고 idle 상태 유지

**의존성**: 없음. 즉시 착수.

---

## 🔴 Phase B — 다중 앵커 RF 공존 (최우선)

**목적**: 앵커 1대 → 5대로 갈 때 충돌 없이 동작하게 한다.
현재 프로토콜은 단일 앵커 가정. 태그는 **아무 앵커**의 poll에 응답하므로
2대 이상이 동시에 같은 태그를 호출하면 충돌 → 패킷 손실 → 거짓 no-range.

**작업 내용**
- RF 모델 결정 (둘 중 택1):
  - **(a) 채널/프리앰블 분리** — DW3000 채널 5/9 활용, 앵커마다 다른 채널 (5대 규모에 현실적)
  - **(b) Listen-Before-Talk + 랜덤 백오프** — 단일 채널 공유 시
- 앵커별 **refresh 예산** 정의: 요구 갱신 주기(예: 200ms) → 태그당 슬롯 시간 → 최대 태그 수 산출
- 5개 보드 전체로 벤치 테스트 (충돌율, no-range율 측정)

**검증 기준**
- 앵커 5대 동시 가동 시 각 태그 no-range율 < 5%
- 한 태그가 2개 앵커 위험구역에 동시에 있어도 양쪽 모두 정상 탐지

**의존성**: Phase A 권장 (필터/타임아웃 정리 후).

---

## Phase C — 필드급 펌웨어 신뢰성

**목적**: 데이터 체인(시각·전달·이벤트)을 신뢰할 수 있게.

**작업 내용**
- **NTP epoch 시각** — 현재 `millis()`(가동시간)는 앵커 간 상관·리부트 후 추적 불가.
  `configTime()`으로 epoch 타임스탬프, 백엔드도 수신 시각 별도 기록
- **ArduinoJson** — 손수 짠 snprintf JSON은 NaN/inf에서 깨짐. 안전한 직렬화로 교체
- **clientId 유일성** — `anchor-{id}` 충돌 시 브로커가 한쪽 강퇴(silent dropout). MAC/chip-ID 추가
- **동적 태그 레지스트리** — 컴파일타임 `TAG_IDS[]` 고정 → 태그가 announce, 앵커가 최근 본 태그만 폴링하고 죽은 태그 age-out
- **앵커 측 이벤트 버퍼링** — LittleFS에 위험 진입/이탈 이벤트 저장, 재연결 시 replay (백엔드 다운 중 로그 공백 방지)
- **TLS** — 1883 평문 → 8883 + 디바이스별 인증서 (현장 WiFi 노출 방지)

**의존성**: Phase A/B 후.

---

## Phase D — 백엔드 + DB

**목적**: 시각이 찍힌 깨끗한 데이터를 권위 있게 영속화.

**작업 내용**
- **브로커 하드닝** — Mosquitto systemd 자동재시작 + 영속화, 또는 관리형(EMQX/HiveMQ)
- **Node.js 인제스트** — MQTT subscribe → **앵커의 relay/danger 전이를 이벤트로 기록**
  (대시보드가 거리로 재계산하지 않음. 앵커가 권위)
- **DB 분리**:
  - **Postgres** (마스터 + 이벤트/컴플라이언스) — SQLite는 PoC 전용
    - 마스터 테이블: `sites`, `machines(anchors)`, `tags`, `workers`, `tag_assignments`(기간 한정)
    - 이벤트: `event_type(enter/exit/relay_on/relay_off/anchor_offline)`, `distance_at_event`, `device_ts`, `server_ts`, `worker_id`
  - **InfluxDB** (거리 시계열) — **retention(원본 ~7일) + 다운샘플 롤업(1분 평균 ~1년)** 필수
    - 태그 키(anchorId, tagId)는 저카디널리티 유지, 고유 ID는 field로
- **인증** — JWT + WSS/HTTPS
- **Staleness 감지** — status가 2×주기 내 없으면 offline 처리 (전달 보장에 의존하지 않음)

**의존성**: Phase C(시각·이벤트 권위) 후.

---

## Phase E — 대시보드 실연동

**목적**: 실데이터로 펌웨어 버그를 조기 노출.

**작업 내용**
- `USE_MOCK: false` 전환, WebSocket 연결
- **Staleness UI** — 데이터 끊기면 마커 회색 처리 + "NO DATA Xs ago" 명확히 표시
  (현재는 멈춘 화면이 "전부 안전"처럼 보임)
- **STOP/RUN을 1순위 지표로** — 워커 점 위치보다 "장비가 실제로 멈췄는가"가 운영 진실
- 위험 판정은 **앵커 flag/relay 상태 신뢰** (클라이언트 재계산 제거)
- 이벤트 로그·히스토리는 **백엔드 API에서** 조회, 클라이언트는 live tail만
- diff 기반 렌더 (현재 `innerHTML` 통째 교체 → 스크롤·트랜지션 손실)

**의존성**: Phase D 후. (대시보드 골격은 이미 존재하므로 빠르게 연결 가능)

---

## Phase F — 측위 & GPS

**목적**: "무엇이 어디 있는가"를 정직하게.

**핵심 인식**: 현재 시스템은 **거리만** 측정, **방위(bearing)는 모름** (PDOA 비활성).
따라서 태그의 지도상 점 위치는 알 수 없다. 대시보드의 태그 오프셋은 illustrative.

**작업 내용**
- **고정/저속 장비**: 측량 좌표(config 값) 또는 GPS 평균 후 고정. 정적 장비에 노이즈 큰 실시간 GPS 스트리밍 지양
- **이동 장비만**: HDOP 게이팅 + 스무딩(Kalman/이동평균) 적용 GPS
- **보드 변종 확인**: GPIO16/17은 ESP32-WROVER에서 PSRAM 핀. WROVER면 GPS UART 충돌 → 보드 변종 먼저 확인
- **워커 방위가 필요하면**: DW3000 **PDOA AoA** 또는 **다중 앵커 삼변측량** — 별도 단계로 의도적 결정
- 그 전까지 대시보드는 근접도를 **거리 링/목록**으로 표현 (점 아님)

**의존성**: Phase E 후. 안전 관련성 가장 낮음.

---

## Phase G — 현장 강화 & 법규 준수

**목적**: 실투입 가능한 내구성·운영·법적 준수.

**작업 내용**

_배터리 & 절전 (Tag)_
- 응답 후 Light Sleep (poll 때 깨어남), 배터리 잔량 응답 프레임 포함, 저전압 경고

_OTA_
- ArduinoOTA / ESP-IDF OTA, 현장 WiFi 원격 업데이트

_알람 & 2단계 구역_
- Tag 부저/진동: 5m 경고, 3m 위험
- Anchor 경광등/사이렌: 릴레이 ON 동시 출력

_하드웨어 내구성_
- IP65 방수방진, 진동/충격 커넥터

_법규 & 개인정보 (PIPA)_
- 작업자 위치는 **개인정보**. 접근통제·보존기한·삭제/익명화 정책
- 산업안전보건법 이벤트 로그 감사 가능성 확보
- 안테나 딜레이 교정 SOP, 캘리브레이션 감사 절차

**의존성**: 전 단계 후.

---

## 기술 스택 (개정)

| 레이어 | 기술 | 비고 / 변경점 |
|---|---|---|
| Anchor 펌웨어 | Arduino (ESP32) | FreeRTOS 이중 코어 |
| Tag 펌웨어 | Arduino (ESP32) | Phase A에서 RX 타임아웃 수정, Phase G 절전 |
| 직렬화 | **ArduinoJson** | 손수 짠 JSON 교체 (Phase C) |
| 시각 | **NTP epoch** | `millis()` 교체 (Phase C) |
| GPS 모듈 | u-blox NEO-6M | 고정 장비는 측량 좌표 선호 (Phase F) |
| MQTT 브로커 | Mosquitto (TLS) / 관리형 | 평문 1883 → 8883 (Phase C/D) |
| 백엔드 | Node.js + Express + MQTT.js + Socket.io | 앵커 전이 = 이벤트 권위 |
| 이벤트/마스터 DB | **PostgreSQL** | SQLite는 PoC 전용으로 강등 |
| 시계열 DB | InfluxDB | retention + 다운샘플 필수 |
| 지도 | Leaflet.js + OpenStreetMap | 근접도는 링/목록 (방위 데이터 전까지) |
| 인증 | JWT + WSS/HTTPS | 신규 (Phase D) |
| VPS | AWS EC2 / DigitalOcean | 단일 장애점 인지, 백업 필수 |

---

## 네트워크 아키텍처

```
[공사현장 WiFi]
  Anchor #1 ──┐
  Anchor #2 ──┤──→ Internet ──→ [MQTT Broker (TLS, VPS)]
  Anchor #3 ──┘                        │
                                [Node.js 인제스트]
                                   │         │
                          [Postgres+Influx]  [WebSocket(WSS)]
                                               │
                                    [Web Dashboard (인증)]
                                    (감독관 PC/모바일)
```

핵심 원칙 (불변): **GPIO26 안전 제어는 서버/네트워크 없이 Anchor 단독으로 동작.**
네트워크는 모니터링 전용이며 안전 기능에 영향을 주지 않는다.

권위 체인: **앵커가 판정·타임스탬프 → 브로커 전송 → 백엔드 영속화(2중 시각) → 대시보드 표시.**
대시보드는 어떤 이벤트의 진실 원천도 아니다.

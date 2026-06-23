# PROJECT_STATUS.md
_Last updated: 2026-06-23_

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
| 안테나 딜레이 | TX/RX 각 16385 (미교정 상태) |

---

## 현재 구현 상태

### ✅ Phase 1 — 기반 안정화 (완료)

- `Sleep()` → `delay()` 수정
- 시리얼 메시지 오류 수정 (`No range x5` → `x20`)
- 미사용 `insideCount`, `CONFIRM_ON_COUNT` 제거
- 워치독 타이머 15초 (Anchor) / 10초 (Tag)
- 거리 이동 평균 필터 5회 (유효값만 누적)
- 부팅 시 Relay OFF 보장

### ✅ Phase 2-A — 다중 태그 UWB 프로토콜 (완료)

- 프레임에 ANCHOR_ID + TAG_ID 필드 추가
- Anchor: 라운드로빈 순차 폴링 (Tag#1→2→3→4→반복)
- Tag: 자신의 MY_TAG_ID에 해당하는 poll에만 응답
- TagState 구조체로 태그별 독립 위험 상태 관리
- 릴레이: 태그 하나라도 위험 → ON / 전부 안전 → OFF
- Tag가 senderAnchorId를 echo → 향후 다중 앵커 대응 가능

### ✅ 안전 독립성 설계 (완료)

- FreeRTOS 이중 코어 분리
  - Core 1: `taskUWB` (우선순위 2) — UWB + GPIO26 전담
  - Core 0: `taskNetwork` (우선순위 1) — WiFi + MQTT 전담
- portMUX로 공유 데이터 보호
- 네트워크 장애가 UWB 안전 루프에 영향 없음을 구조적으로 보장
- WDT를 `taskUWB` 내부에서 등록 (setup 태스크 삭제 후 WDT 오동작 방지)

### ✅ Phase 2-B — WiFi + MQTT (완료)

- `taskNetwork`에 WiFi 연결 + 자동 재연결 구현
- PubSubClient 기반 MQTT publish
- LWT(Last Will Testament) 설정 — 비정상 종료 시 브로커가 offline 발행
- 릴레이 상태 변경 즉시 publish (retain=true)
- 거리 데이터 1초 주기 publish
- 헬스체크(status) 30초 주기 publish (retain=true)
- MQTT 재연결 5초 간격, 비차단(non-blocking)

---

## 파일 구조

```
UWB/
├── Anchor/ancher_v1/ancher_v1.ino   Anchor 펌웨어 (현재 Phase 2-B)
├── Tag/tag_v1/tag_v1.ino            Tag 펌웨어 (Phase 2-A)
├── PROJECT_STATUS.md                이 파일
├── ROADMAP.md                       전체 로드맵
└── CLAUDE_CONTEXT.md                Claude 인스턴스용 컨텍스트
```

---

## MQTT 토픽 구조 (현재)

```
uwb/anchor/{id}/tag/{tag_id}/range  — 거리 데이터, 1초 주기
uwb/anchor/{id}/relay               — 릴레이 상태, 변경 즉시, retain
uwb/anchor/{id}/status              — 헬스체크, 30초 주기, retain
```

페이로드 예시:
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
| 전체 사이클 (태그 4개) | 600ms |
| 각 태그 측정 주기 | ~1.67Hz |
| RX 타임아웃 | 3ms |

---

## 알려진 한계 / 미해결 사항

| # | 항목 | 내용 |
|---|---|---|
| 1 | 안테나 딜레이 미교정 | TX/RX ANT DLY = 16385 기본값. 실측 교정 시 거리 정확도 향상 가능 |
| 2 | 태그 정적 등록 | `TAG_IDS[]` 컴파일 타임 고정. 태그 추가 시 재플래시 필요 |
| 3 | `g_reportTimestamp` | 기록되지만 대시보드 미구현으로 아직 미활용 |
| 4 | Tag WDT | Anchor 전원 off 시 Tag spin-wait → WDT 재부팅. 의도된 동작이나 현장 확인 필요 |
| 5 | GPS 미연결 | Phase 2-C에서 구현 예정. 현재 Anchor 위치 정보 없음 |

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

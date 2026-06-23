# ROADMAP.md
_공사현장 UWB 안전 시스템 — 단계별 구현 계획_

---

## 목표

실제 공사현장에서 즉시 투입 가능한 UWB 기반 작업자 안전거리 감지 시스템.
- 굴착기 등 위험 장비에 Anchor 부착
- 작업자가 Tag 착용
- 3m 이내 접근 시 장비 자동 정지 (GPIO26 릴레이)
- 현장 감독관이 웹 브라우저로 실시간 모니터링

---

## 전체 단계 현황

```
Phase 1    기반 안정화                    ✅ 완료
Phase 2-A  다중 태그 UWB 프로토콜         ✅ 완료
Phase 2-B  Anchor WiFi + MQTT            ✅ 완료
────────────────────────────────────────────────────
Phase 2-C  Anchor GPS 연동               ⬜ 미구현
Phase 3    백엔드 서버                    ⬜ 미구현
Phase 4    웹 대시보드                    ⬜ 미구현
Phase 5    현장 강화                      ⬜ 미구현
```

---

## 상세 계획

---

### ✅ Phase 1 — 기반 안정화

**작업 내용**
- 버그 수정 (`Sleep()` → `delay()`, 시리얼 메시지 오류)
- 미사용 변수 제거 (`insideCount`, `CONFIRM_ON_COUNT`)
- 워치독 타이머 추가
- 거리 이동 평균 필터 (5회)
- 부팅 시 Relay OFF 보장

---

### ✅ Phase 2-A — 다중 태그 UWB 프로토콜

**작업 내용**
- 프레임에 ANCHOR_ID + TAG_ID 추가
- Anchor 라운드로빈 폴링
- Tag 자신의 ID에만 응답
- 태그별 독립 위험 상태 관리
- 다중 태그 릴레이 로직

---

### ✅ Phase 2-B — Anchor WiFi + MQTT

**작업 내용**
- FreeRTOS 이중 코어 분리 (UWB/네트워크 완전 독립)
- WiFi 연결 + 자동 재연결
- PubSubClient MQTT publish
- LWT, retain 메시지 설계
- 릴레이 변경 즉시 publish / 거리 1초 주기 / 헬스체크 30초

---

### ⬜ Phase 2-C — Anchor GPS 연동

**목적**: Anchor의 실제 위치(위경도)를 서버로 전송 → 지도에 표시
GPS는 지도 표시 전용. 안전 거리 측정은 UWB만 담당.

**작업 내용**
- u-blox NEO-6M 또는 NEO-M8N GPS 모듈 연결 (UART)
- TinyGPS++ 라이브러리 파싱
- GPS 데이터를 `taskNetwork`에서 MQTT publish

**추가 MQTT 토픽**
```
uwb/anchor/{id}/gps  →  {"lat":37.123,"lon":127.456,"alt":12.5,"ts":123456}
```

**하드웨어 연결 (예정)**
```
NEO-6M TX  →  ESP32 GPIO16 (RX2)
NEO-6M RX  →  ESP32 GPIO17 (TX2)
NEO-6M VCC →  3.3V
NEO-6M GND →  GND
```

**의존성**: Phase 2-B 완료 후 진행

---

### ⬜ Phase 3 — 백엔드 서버

**목적**: MQTT 데이터 수신 → DB 저장 → WebSocket으로 대시보드에 전달

**작업 내용**
- MQTT 브로커 설치 (Mosquitto on VPS)
- Node.js + Express 서버
- MQTT.js로 브로커 subscribe
- Socket.io WebSocket 서버 (대시보드 실시간 push)
- InfluxDB (시계열 거리 데이터)
- SQLite (이벤트 로그: 위험 진입/이탈 기록)

**API 엔드포인트 (예정)**
```
GET  /api/anchors           — 등록된 앵커 목록 + 마지막 상태
GET  /api/anchors/:id/logs  — 이벤트 로그 조회
WS   /                      — 실시간 데이터 스트림
```

**인프라**
```
VPS (공인 IP) 위에:
  - Mosquitto MQTT 브로커  (port 1883)
  - Node.js 서버           (port 3000)
  - InfluxDB               (port 8086)
```

**의존성**: Phase 2-B 완료 후 독립적으로 진행 가능

---

### ⬜ Phase 4 — 웹 대시보드

**목적**: 현장 감독관이 PC/스마트폰으로 실시간 확인

**작업 내용**
- Leaflet.js + OpenStreetMap (또는 카카오맵) 지도
- Anchor GPS 위치 → 지도에 마커
- 릴레이 ON/OFF → 마커 색상 변경 (정상: 초록, 위험: 빨강)
- Anchor 주변 3m 위험 구역 원(circle) 표시
- 태그별 상태 패널 (거리, 위험 여부, 마지막 업데이트)
- 위험 진입 시 브라우저 알림 + 경고음
- 이벤트 로그 테이블

**의존성**: Phase 3 완료 후 진행

---

### ⬜ Phase 5 — 현장 강화

**목적**: 실사용 투입을 위한 내구성 및 편의성 확보

**작업 내용**

_배터리 & 절전 (Tag)_
- 응답 후 Light Sleep 모드 (Anchor poll 때 깨어남)
- 배터리 잔량을 응답 프레임에 포함
- 저전압 경고 MQTT publish

_OTA 펌웨어 업데이트_
- ArduinoOTA 또는 ESP-IDF OTA
- 현장 WiFi 통해 원격 업데이트

_알람 & 경보 (Phase 3 연동)_
- Tag에 부저/진동 추가: 5m 경고 구역 → 부저
- Anchor에 경광등/사이렌 추가: 릴레이 ON 시 동시 출력
- 2단계 구역: 경고(5m) + 위험(3m)

_하드웨어 내구성_
- IP65 방수방진 케이스
- 진동/충격 내구성 커넥터

_운영_
- 안테나 딜레이 교정 (실측 기반)
- 산업안전보건법 관련 이벤트 로그 기록 확인

---

## 기술 스택 확정

| 레이어 | 기술 | 비고 |
|---|---|---|
| Anchor 펌웨어 | Arduino (ESP32) | FreeRTOS 이중 코어 |
| Tag 펌웨어 | Arduino (ESP32) | Phase 5에서 절전 추가 |
| GPS 모듈 | u-blox NEO-6M | TinyGPS++ 파싱 |
| MQTT 브로커 | Mosquitto | VPS 자체 호스팅 |
| 백엔드 | Node.js + Express + MQTT.js + Socket.io | |
| 시계열 DB | InfluxDB | 거리 데이터 |
| 이벤트 DB | SQLite | 위험 진입/이탈 로그 |
| 지도 | Leaflet.js + OpenStreetMap | 카카오맵으로 교체 가능 |
| VPS | AWS EC2 t2.micro 또는 DigitalOcean $6/월 | 공인 IP 필수 |

---

## 네트워크 아키텍처

```
[공사현장 WiFi]
  Anchor #1 ──┐
  Anchor #2 ──┤──→ Internet ──→ [MQTT Broker (VPS)]
  Anchor #3 ──┘                        │
                                [Node.js Server]
                                   │         │
                              [InfluxDB]  [WebSocket]
                                               │
                                    [Web Dashboard]
                                    (감독관 PC/모바일)
```

핵심 원칙: **GPIO26 안전 제어는 서버/네트워크 없이 Anchor 단독으로 동작.**
네트워크는 모니터링 전용이며 안전 기능에 영향을 주지 않는다.

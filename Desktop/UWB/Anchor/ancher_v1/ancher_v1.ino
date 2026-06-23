#include "dw3000.h"
#include "SPI.h"
#include "esp_task_wdt.h"
#include <WiFi.h>
#include <PubSubClient.h>

extern SPISettings _fastSPI;
extern dwt_txconfig_t txconfig_options;

/* ── 핀 설정 ── */
#define PIN_RST 27
#define PIN_IRQ 34
#define PIN_SS  4
#define RELAY_PIN 26
#define RELAY_ACTIVE_LOW 0

/* ── 디바이스 설정 (앵커마다 ANCHOR_ID 변경) ── */
#define ANCHOR_ID 1

/* ── 등록된 태그 ID 목록 ── */
static const uint8_t TAG_IDS[] = {1, 2, 3, 4};
static const int     NUM_TAGS  = sizeof(TAG_IDS) / sizeof(TAG_IDS[0]);

/* ── 안전 거리 파라미터 ── */
#define ENTER_DISTANCE_M   3.00
#define EXIT_DISTANCE_M    3.20
#define CONFIRM_OFF_COUNT  3
#define NO_RANGE_OFF_COUNT 20
#define RNG_DELAY_MS       150

/* ── DW3000 타이밍 ── */
/* ⚠️ 안테나 딜레이 교정 (리뷰 C5) — 미교정 시 ±10~30cm 오차, 3m 임계값에서 늦은 정지 위험.
 *    2점 교정 SOP:
 *      1) 태그를 정확히 알려진 거리(예: 2.00m, 5.00m)에 고정
 *      2) 시리얼 raw 거리 평균을 읽어 오차(측정-실제) 계산
 *      3) 오차 1cm ≈ ANT_DLY 약 0.5 LSB. 측정이 멀게 나오면 ANT_DLY를 키움
 *      4) TX_ANT_DLY = RX_ANT_DLY 동일 적용, 두 거리에서 모두 ±5cm 들도록 반복
 *    교정값은 보드(안테나)마다 다르므로 디바이스별로 따로 둘 것. */
#define TX_ANT_DLY              16385
#define RX_ANT_DLY              16385
#define POLL_TX_TO_RESP_RX_DLY_UUS 240
#define RESP_RX_TIMEOUT_UUS        3000

/* ── 워치독 / 필터 ──
 * 비대칭 median 필터 (리뷰 C1):
 *   진입(릴레이 ON) = 최근 ENTER_FILTER_SIZE개 median → 빠른 반응
 *   이탈(릴레이 OFF) = 최근 DIST_FILTER_SIZE개 median → 신중 (멀티패스 스파이크 내성)
 * median은 평균과 달리 단발 스파이크에 끌려가지 않는다. */
#define WDT_TIMEOUT_S      15
#define DIST_FILTER_SIZE    5   /* 이탈/표시용 윈도우 */
#define ENTER_FILTER_SIZE   3   /* 진입용 윈도우 (빠른 안전 반응) */

/* ── Phase B: 적응형 폴링 ──
 * 꺼진 태그(장기 no-range)를 슬로 폴링으로 전환해 활성 태그 사이클 단축.
 * 하드웨어 확정: 앵커 1대 + 태그 4대. 앵커 간 RF 충돌(C3) 해당 없음.
 *
 * SLOW_POLL_THRESHOLD : 비위험 상태에서 연속 no-range N회 → 슬로 전환
 *                       (10 × 150ms × 4태그 ≈ 6s 응답 없으면 부재로 간주)
 * SLOW_POLL_PERIOD    : 슬로 중 N사이클마다 1회만 실제 폴링
 *                       (5사이클 = 재발견 체크 주기 ≈ 활성태그수 × 150ms × 5)
 *
 * 효과 예시 (태그 3활성 1꺼짐):
 *   정상: 4 × 150ms = 600ms 사이클
 *   슬로: 활성 3태그 ≈ 450ms 사이클, 꺼진 태그 5사이클마다 1회 체크
 *
 * SAFETY: 위험 상태(inDanger=true) 태그는 슬로 폴링 전환 안 함.
 *         꺼진 줄 알았다가 돌아왔을 때 빠르게 감지해야 하기 때문. */
#define SLOW_POLL_THRESHOLD 10
#define SLOW_POLL_PERIOD     5

/* ── 프레임 필드 인덱스 ──
 *  [0-1] 프레임 타입  [2] SEQ  [3-4] PAN ID
 *  [5] ANCHOR_ID      [6] TAG_ID  [7] MSG_TYPE
 *  [8-9] 패딩         [10-13] poll_rx_ts  [14-17] resp_tx_ts
 */
#define ALL_MSG_SN_IDX          2
#define MSG_ANCHOR_ID_IDX       5
#define MSG_TAG_ID_IDX          6
#define MSG_TYPE_IDX            7
#define RESP_MSG_POLL_RX_TS_IDX 10
#define RESP_MSG_RESP_TX_TS_IDX 14

/* ═══════════════════════════════════════
 *  Phase 2-B: WiFi / MQTT 설정
 *  ← 현장 환경에 맞게 수정
 * ═══════════════════════════════════════ */
#define WIFI_SSID      "your_ssid"
#define WIFI_PASSWORD  "your_password"
#define MQTT_BROKER    "192.168.1.100"   /* 브로커 IP 또는 도메인 */
#define MQTT_PORT      1883
#define MQTT_USER      ""                /* 인증 없으면 빈 문자열 */
#define MQTT_PASS      ""

/* publish 주기 */
#define RANGE_PUB_MS   1000
#define STATUS_PUB_MS  30000
#define MQTT_RETRY_MS  5000
/* ══════════════════════════════════════ */

static dwt_config_t config = {
  5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1,
  DWT_BR_6M8, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
  (129 + 8 - 8), DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0
};

static uint8_t tx_poll_msg[] = {
  0x41, 0x88, 0, 0xCA, 0xDE,
  0, 0, 0xE0, 0, 0
};

static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[20];
static uint32_t status_reg = 0;

bool relayState    = false;
int  currentTagIdx = 0;

/* ── 태그별 상태 ── */
struct TagState {
  uint8_t id;
  bool    inDanger;
  int     outsideCount;
  int     noRangeCount;   /* 안전 타임아웃용: inDanger 중 no-range 횟수 */
  double  distBuf[DIST_FILTER_SIZE];
  int     distIdx;
  int     distCount;
  double  lastFiltered;
  int     absentCount;    /* Phase B: 비위험 중 연속 no-range (슬로 폴링 판단) */
  uint8_t pollSkip;       /* Phase B: 남은 스킵 사이클 (0=이번 사이클 폴링) */
};

static TagState tagStates[8];

/* ────────────────────────────────────────────────────────
 *  Core 간 공유 데이터
 *  Core 1(UWB) 쓰기 전용 / Core 0(Network) 읽기 전용
 * ──────────────────────────────────────────────────────── */
struct TagReport {
  uint8_t tagId;
  float   distRaw;       /* -1 = no range */
  float   distFiltered;
  bool    inDanger;
  bool    absent;        /* Phase B: 슬로 폴링 중 = 태그 부재로 추정 */
};

volatile bool      g_relayState         = false;
volatile TagReport g_tagReports[8]      = {};
volatile uint32_t  g_reportTimestamp[8] = {};
portMUX_TYPE       g_reportMux          = portMUX_INITIALIZER_UNLOCKED;

/* ════════════════════════════════════════════════════════
 *  UWB 안전 함수 (Core 1 전용)
 * ════════════════════════════════════════════════════════ */

/* 링 버퍼에 raw 1개 push */
static void pushSample(TagState &t, double raw)
{
  t.distBuf[t.distIdx] = raw;
  t.distIdx = (t.distIdx + 1) % DIST_FILTER_SIZE;
  if (t.distCount < DIST_FILTER_SIZE) t.distCount++;
}

/* 최근 n개 샘플의 median. n은 보유 샘플 수로 클램프.
 * (멀티패스 스파이크 내성: 평균과 달리 단발 이상치에 끌려가지 않음) */
static double medianOfLast(TagState &t, int n)
{
  if (n > t.distCount) n = t.distCount;
  if (n <= 0) return -1.0;

  double tmp[DIST_FILTER_SIZE];
  for (int k = 0; k < n; k++) {
    int idx = (t.distIdx - 1 - k + DIST_FILTER_SIZE * 2) % DIST_FILTER_SIZE;
    tmp[k]  = t.distBuf[idx];
  }
  /* 삽입 정렬 (n ≤ 5) */
  for (int i = 1; i < n; i++) {
    double key = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = key;
  }
  return tmp[n / 2];
}

static int  relayOnLevel()  { return RELAY_ACTIVE_LOW ? LOW  : HIGH; }
static int  relayOffLevel() { return RELAY_ACTIVE_LOW ? HIGH : LOW;  }

static void setRelay(bool on)
{
  relayState   = on;
  g_relayState = on;
  digitalWrite(RELAY_PIN, on ? relayOnLevel() : relayOffLevel());
}

static void relayInit()
{
  digitalWrite(RELAY_PIN, relayOffLevel());
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);
}

static void updateRelay()
{
  bool anyDanger = false;
  for (int i = 0; i < NUM_TAGS; i++) {
    if (tagStates[i].inDanger) { anyDanger = true; break; }
  }
  if (anyDanger == relayState) return;
  setRelay(anyDanger);
  Serial.println(anyDanger ? ">>> RELAY ON" : ">>> RELAY OFF (all safe)");
}

static void updateReport(int idx, float raw, float filtered, bool danger, bool absent)
{
  portENTER_CRITICAL(&g_reportMux);
  g_tagReports[idx]      = {TAG_IDS[idx], raw, filtered, danger, absent};
  g_reportTimestamp[idx] = (uint32_t)millis();
  portEXIT_CRITICAL(&g_reportMux);
}

static void processTagDistance(int idx, double rawDist)
{
  TagState &t = tagStates[idx];
  if (rawDist <= 0.0 || rawDist > 30.0) return;

  t.noRangeCount = 0;
  t.absentCount  = 0;   /* Phase B: 태그 응답 → 부재 카운터 리셋 */
  t.pollSkip     = 0;   /* Phase B: 슬로 폴링 중이었다면 즉시 정상 복귀 */
  pushSample(t, rawDist);

  /* 비대칭 필터 (리뷰 C1):
   *   enterDist = 짧은 윈도우 median → 위험 진입을 빠르게 감지 (안전은 빠르게)
   *   exitDist  = 긴 윈도우 median  → 이탈/표시는 신중하게 (복구는 신중하게) */
  double enterDist = medianOfLast(t, ENTER_FILTER_SIZE);
  double exitDist  = medianOfLast(t, DIST_FILTER_SIZE);
  t.lastFiltered   = exitDist;

  if (!t.inDanger) {
    if (enterDist <= ENTER_DISTANCE_M) {
      t.inDanger     = true;
      t.outsideCount = 0;
      Serial.printf("[Tag#%d] DANGER ENTER %.2fm -> RELAY ON\n", t.id, enterDist);
      setRelay(true);
    }
  } else {
    if (exitDist >= EXIT_DISTANCE_M) {
      t.outsideCount++;
      Serial.printf("[Tag#%d] outsideCount: %d\n", t.id, t.outsideCount);
      if (t.outsideCount >= CONFIRM_OFF_COUNT) {
        t.inDanger     = false;
        t.outsideCount = 0;
        Serial.printf("[Tag#%d] SAFE EXIT\n", t.id);
        updateRelay();
      }
    } else {
      t.outsideCount = 0;
    }
  }
  updateReport(idx, (float)rawDist, (float)t.lastFiltered, t.inDanger, false);
}

static void processNoRange(int idx)
{
  TagState &t = tagStates[idx];

  /* ── 위험 상태 중 no-range: 안전 타임아웃만 처리 ──
   * 위험 중엔 슬로 폴링으로 전환하지 않는다.
   * 꺼진 줄 알았던 태그가 돌아올 때 최대한 빠르게 감지해야 하기 때문. */
  if (t.inDanger) {
    updateReport(idx, -1.0f, -1.0f, true, false);
    t.noRangeCount++;
    Serial.printf("[Tag#%d] noRangeCount: %d\n", t.id, t.noRangeCount);
    if (t.noRangeCount >= NO_RANGE_OFF_COUNT) {
      t.inDanger     = false;
      t.noRangeCount = 0;
      t.outsideCount = 0;
      Serial.printf("[Tag#%d] NO RANGE TIMEOUT -> SAFE\n", t.id);
      updateRelay();
    }
    return;
  }

  /* ── 비위험 상태 중 no-range: Phase B 슬로 폴링 판단 ── */
  t.absentCount++;
  bool nowAbsent = (t.absentCount >= SLOW_POLL_THRESHOLD);
  updateReport(idx, -1.0f, -1.0f, false, nowAbsent);

  if (nowAbsent) {
    /* 슬로 폴링: 다음 SLOW_POLL_PERIOD-1 사이클은 건너뜀 */
    t.pollSkip = SLOW_POLL_PERIOD - 1;
    if (t.absentCount == SLOW_POLL_THRESHOLD) {
      Serial.printf("[Tag#%d] 슬로 폴링 전환 (absent x%d) — 사이클 단축\n",
                    t.id, t.absentCount);
    }
  }
}

/* ════════════════════════════════════════════════════════
 *  MQTT publish 헬퍼 (Core 0 전용)
 *
 *  MQTT 토픽 구조:
 *    uwb/anchor/{id}/tag/{tag_id}/range  — 거리 데이터 (1초 주기)
 *    uwb/anchor/{id}/relay               — 릴레이 상태 변경 즉시, retain
 *    uwb/anchor/{id}/status              — 헬스체크 30초 주기, retain
 * ════════════════════════════════════════════════════════ */

static void publishRange(PubSubClient &mqtt)
{
  /* 크리티컬 섹션에서 빠르게 로컬 복사 후 섹션 밖에서 publish */
  TagReport localReports[8];
  uint32_t  localTs[8];

  portENTER_CRITICAL(&g_reportMux);
  for (int i = 0; i < NUM_TAGS; i++) {
    localReports[i] = (TagReport)g_tagReports[i];
    localTs[i]      = g_reportTimestamp[i];
  }
  portEXIT_CRITICAL(&g_reportMux);

  char topic[64], payload[128];
  for (int i = 0; i < NUM_TAGS; i++) {
    TagReport &r = localReports[i];
    snprintf(topic, sizeof(topic),
             "uwb/anchor/%d/tag/%d/range", ANCHOR_ID, r.tagId);

    if (r.distRaw < 0.0f) {
      snprintf(payload, sizeof(payload),
               "{\"raw\":null,\"filt\":null,\"danger\":%s,\"absent\":%s,\"ts\":%lu}",
               r.inDanger ? "true" : "false",
               r.absent   ? "true" : "false",
               (unsigned long)localTs[i]);
    } else {
      snprintf(payload, sizeof(payload),
               "{\"raw\":%.2f,\"filt\":%.2f,\"danger\":%s,\"absent\":false,\"ts\":%lu}",
               r.distRaw, r.distFiltered,
               r.inDanger ? "true" : "false",
               (unsigned long)localTs[i]);
    }
    mqtt.publish(topic, payload, false);
  }
}

static void publishRelay(PubSubClient &mqtt, bool state)
{
  char topic[48], payload[64];
  snprintf(topic,   sizeof(topic),   "uwb/anchor/%d/relay", ANCHOR_ID);
  snprintf(payload, sizeof(payload),
           "{\"state\":%s,\"ts\":%lu}",
           state ? "true" : "false", (unsigned long)millis());
  mqtt.publish(topic, payload, true); /* retain — 대시보드 접속 시 마지막 상태 즉시 수신 */
}

static void publishStatus(PubSubClient &mqtt, bool online)
{
  char topic[48], payload[128];
  snprintf(topic, sizeof(topic), "uwb/anchor/%d/status", ANCHOR_ID);
  snprintf(payload, sizeof(payload),
           "{\"online\":%s,\"anchor_id\":%d,\"tags\":%d,\"uptime\":%lu,\"ts\":%lu}",
           online ? "true" : "false",
           ANCHOR_ID, NUM_TAGS,
           (unsigned long)(millis() / 1000),
           (unsigned long)millis());
  mqtt.publish(topic, payload, true); /* retain */
}

/* ════════════════════════════════════════════════════════
 *  Core 0 태스크 — 네트워크 리포팅
 *
 *  SAFETY: 이 태스크가 죽거나 WiFi/MQTT가 끊겨도
 *          Core 1의 UWB 안전 루프와 GPIO26 릴레이는
 *          완전히 독립적으로 동작한다.
 * ════════════════════════════════════════════════════════ */
static void taskNetwork(void *pvParameters)
{
  static WiFiClient   wifiClient;
  static PubSubClient mqtt(wifiClient);

  /* LWT: 앵커가 비정상 종료되면 브로커가 offline 메시지를 자동 발행 */
  char lwt_topic[48];
  snprintf(lwt_topic, sizeof(lwt_topic), "uwb/anchor/%d/status", ANCHOR_ID);
  const char *lwt_payload = "{\"online\":false}";

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(5);

  /* ── WiFi 초기 연결 (최대 15초 대기) ── */
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[Net] WiFi 연결 중");
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[Net] WiFi 연결됨: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[Net] WiFi 초기 연결 실패 — 백그라운드 재시도");
  }

  uint32_t lastMqttRetry = 0;
  uint32_t lastRangePub  = 0;
  uint32_t lastStatusPub = 0;
  bool     lastRelay     = false;

  for (;;) {
    /* ── WiFi 체크 ── */
    if (WiFi.status() != WL_CONNECTED) {
      /* setAutoReconnect(true)가 처리하므로 로그만 출력 */
      Serial.println("[Net] WiFi 끊김 — 재연결 대기중");
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    /* ── MQTT 재연결 ── */
    if (!mqtt.connected()) {
      uint32_t now = millis();
      if (now - lastMqttRetry >= MQTT_RETRY_MS) {
        lastMqttRetry = now;

        char clientId[32];
        snprintf(clientId, sizeof(clientId), "anchor-%d", ANCHOR_ID);

        bool ok = (strlen(MQTT_USER) > 0)
          ? mqtt.connect(clientId, MQTT_USER, MQTT_PASS, lwt_topic, 1, true, lwt_payload)
          : mqtt.connect(clientId, NULL, NULL,            lwt_topic, 1, true, lwt_payload);

        if (ok) {
          Serial.printf("[Net] MQTT 연결됨: %s:%d\n", MQTT_BROKER, MQTT_PORT);
          publishStatus(mqtt, true); /* 온라인 알림 즉시 발행 */
          lastStatusPub = millis();
        } else {
          Serial.printf("[Net] MQTT 연결 실패 (rc=%d) — %dms 후 재시도\n",
                        mqtt.state(), MQTT_RETRY_MS);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    mqtt.loop(); /* 브로커 keep-alive 처리 */

    uint32_t now = millis();

    /* 릴레이 상태 변경 → 즉시 publish */
    bool curRelay = g_relayState;
    if (curRelay != lastRelay) {
      publishRelay(mqtt, curRelay);
      lastRelay = curRelay;
      Serial.printf("[Net] relay publish: %s\n", curRelay ? "ON" : "OFF");
    }

    /* 1초마다 거리 데이터 publish */
    if (now - lastRangePub >= RANGE_PUB_MS) {
      publishRange(mqtt);
      lastRangePub = now;
    }

    /* 30초마다 헬스체크 publish */
    if (now - lastStatusPub >= STATUS_PUB_MS) {
      publishStatus(mqtt, true);
      lastStatusPub = now;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/* ════════════════════════════════════════════════════════
 *  Core 1 태스크 — UWB 안전 루프
 *
 *  SAFETY: WiFi / MQTT / 서버 상태와 완전히 독립.
 *          네트워크가 없어도 UWB 측정과 GPIO26 릴레이
 *          제어는 중단 없이 동작한다.
 * ════════════════════════════════════════════════════════ */
static void taskUWB(void *pvParameters)
{
  esp_task_wdt_add(NULL); /* 이 태스크를 WDT 감시 대상으로 등록 */

  _fastSPI = SPISettings(16000000L, MSBFIRST, SPI_MODE0);
  spiBegin(PIN_IRQ, PIN_RST);
  spiSelect(PIN_SS);
  delay(2);

  while (!dwt_checkidlerc()) { Serial.println("IDLE FAILED"); delay(100); }
  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) { Serial.println("INIT FAILED"); while (1); }

  dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
  if (dwt_configure(&config)) { Serial.println("CONFIG FAILED"); while (1); }

  dwt_configuretxrf(&txconfig_options);
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);
  dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
  dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

  Serial.println("[UWB] task started on Core 1");

  for (;;) {
    esp_task_wdt_reset();

    /* Phase B: 슬로 폴링 — 장기 부재 태그는 이번 사이클 건너뜀.
     * delay 없이 즉시 다음 태그로 이동 → 활성 태그 사이클 단축.
     * SAFETY: 위험 상태 태그는 processNoRange에서 슬로 전환 안 하므로
     *         여기서 건너뛰어지는 일이 없다. */
    if (tagStates[currentTagIdx].pollSkip > 0) {
      tagStates[currentTagIdx].pollSkip--;
      currentTagIdx = (currentTagIdx + 1) % NUM_TAGS;
      continue;
    }

    uint8_t targetTagId = TAG_IDS[currentTagIdx];

    tx_poll_msg[ALL_MSG_SN_IDX]    = frame_seq_nb;
    tx_poll_msg[MSG_ANCHOR_ID_IDX] = ANCHOR_ID;
    tx_poll_msg[MSG_TAG_ID_IDX]    = targetTagId;

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
    dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
    dwt_writetxfctrl(sizeof(tx_poll_msg), 0, 1);
    dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

    while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
             (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR))) {}

    frame_seq_nb++;

    if (status_reg & SYS_STATUS_RXFCG_BIT_MASK) {
      dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
      uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;

      if (frame_len <= sizeof(rx_buffer)) {
        dwt_readrxdata(rx_buffer, frame_len, 0);
        rx_buffer[ALL_MSG_SN_IDX] = 0;

        if (rx_buffer[MSG_ANCHOR_ID_IDX] == ANCHOR_ID  &&
            rx_buffer[MSG_TAG_ID_IDX]    == targetTagId &&
            rx_buffer[MSG_TYPE_IDX]      == 0xE1) {

          uint32_t poll_tx_ts = dwt_readtxtimestamplo32();
          uint32_t resp_rx_ts = dwt_readrxtimestamplo32();
          float    clockOffsetRatio = ((float)dwt_readclockoffset()) / (uint32_t)(1 << 26);

          uint32_t poll_rx_ts, resp_tx_ts_val;
          resp_msg_get_ts(&rx_buffer[RESP_MSG_POLL_RX_TS_IDX], &poll_rx_ts);
          resp_msg_get_ts(&rx_buffer[RESP_MSG_RESP_TX_TS_IDX], &resp_tx_ts_val);

          int32_t rtd_init = resp_rx_ts  - poll_tx_ts;
          int32_t rtd_resp = resp_tx_ts_val - poll_rx_ts;
          double  tof      = ((rtd_init - rtd_resp * (1 - clockOffsetRatio)) / 2.0) * DWT_TIME_UNITS;
          double  dist     = tof * SPEED_OF_LIGHT;

          processTagDistance(currentTagIdx, dist);

          Serial.printf("[Tag#%d] raw:%.2fm filt:%.2fm danger:%s relay:%s\n",
                        targetTagId, dist,
                        tagStates[currentTagIdx].lastFiltered,
                        tagStates[currentTagIdx].inDanger ? "Y" : "N",
                        relayState ? "ON" : "OFF");
        }
      }
    } else {
      dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
      processNoRange(currentTagIdx);
      Serial.printf("[Tag#%d] no range  relay:%s\n", targetTagId, relayState ? "ON" : "OFF");
    }

    currentTagIdx = (currentTagIdx + 1) % NUM_TAGS;
    vTaskDelay(pdMS_TO_TICKS(RNG_DELAY_MS));
  }
}

/* ────────────────────────────────── */

void setup()
{
  UART_init();
  Serial.begin(115200);

  relayInit();

  for (int i = 0; i < NUM_TAGS; i++) {
    tagStates[i] = {TAG_IDS[i], false, 0, 0, {0}, 0, 0, 0.0, 0, 0};
    /*              id  danger  out  noR  buf  idx  cnt  filt  abs  skip */
  }

  /* WDT 초기화 — 실제 등록은 각 태스크 내부에서 수행 */
  esp_task_wdt_init(WDT_TIMEOUT_S, true);

  Serial.println();
  Serial.println("=================================");
  Serial.printf( "UWB ANCHOR #%d  (Phase B)\n", ANCHOR_ID);
  Serial.printf( "Tags     : %d\n", NUM_TAGS);
  Serial.printf( "Safety   : %.1fm / %.1fm (enter/exit)\n", ENTER_DISTANCE_M, EXIT_DISTANCE_M);
  Serial.printf( "Filter   : median asym (enter=%d, exit=%d)\n", ENTER_FILTER_SIZE, DIST_FILTER_SIZE);
  Serial.printf( "Cycle    : %dms/tag  max %dms\n", RNG_DELAY_MS, NUM_TAGS * RNG_DELAY_MS);
  Serial.printf( "SlowPoll : absent x%d -> 1/%d rate\n", SLOW_POLL_THRESHOLD, SLOW_POLL_PERIOD);
  Serial.printf( "MQTT     : %s:%d\n", MQTT_BROKER, MQTT_PORT);
  Serial.println("=================================");

  /* Core 1 — UWB 안전 루프 (최우선, 네트워크 독립) */
  xTaskCreatePinnedToCore(
    taskUWB, "UWB_Safety",
    8192, NULL,
    2,          /* 우선순위 높음 */
    NULL, 1     /* Core 1 고정 */
  );

  /* Core 0 — 네트워크 리포팅 (낮은 우선순위, 실패 무방)
   *
   * SAFETY: taskNetwork는 의도적으로 panic WDT에 등록하지 않는다.
   *   panic=true WDT에 묶으면 네트워크 hang(DNS/소켓 지연)이 칩 전체를
   *   리부트시켜 Core 1 안전 루프까지 중단시킨다 — 안전 독립성 위배.
   *   네트워크 hang은 socketTimeout(5s)+재연결 로직으로 자체 복구한다. */
  xTaskCreatePinnedToCore(
    taskNetwork, "Network",
    8192,        /* WiFi/TLS 스택 고려해 여유있게 */
    NULL,
    1,           /* 우선순위 낮음 */
    NULL, 0      /* Core 0 고정 */
  );
}

void loop()
{
  vTaskDelete(NULL); /* loop()는 사용하지 않음 */
}

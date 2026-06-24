#include "dw3000.h"
#include "SPI.h"
#include "esp_task_wdt.h"
#include "esp_err.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <string.h>

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

/* ── 등록된 태그 ID 목록 ──
 * 실제 켜져 있는 태그만 넣으세요. 지금 Tag 코드가 MY_TAG_ID 2 하나라면 {2}만 권장.
 * {2, 3}으로 두고 Tag#3 보드가 꺼져 있으면 Tag#3은 정상적으로 no range가 납니다. */
static const uint8_t TAG_IDS[] = {2,3,4};
static const int     NUM_TAGS  = sizeof(TAG_IDS) / sizeof(TAG_IDS[0]);

/* ── 안전 거리 파라미터 ── */
#define ENTER_DISTANCE_M   3.00
#define EXIT_DISTANCE_M    3.20
#define CONFIRM_OFF_COUNT  3
#define NO_RANGE_OFF_COUNT 20
#define RNG_DELAY_MS       150

/* ── DW3000 타이밍 ──
 * Makerfabs 기본 예제는 SPI 7MHz, softreset, 12-byte poll / 20-byte response를 사용한다.
 * 아래 값은 응답 지연을 넉넉하게 잡은 디버그 안정화용 설정. */
#define SPI_HZ                     7000000L
#define TX_ANT_DLY                 16385
#define RX_ANT_DLY                 16385
#define POLL_TX_TO_RESP_RX_DLY_UUS 500
#define RESP_RX_TIMEOUT_UUS        5000

/* ── 워치독 / 필터 ── */
#define WDT_TIMEOUT_S      15
#define DIST_FILTER_SIZE    5
#define ENTER_FILTER_SIZE   3

/* ── Phase B: 적응형 폴링 ── */
#define SLOW_POLL_THRESHOLD 10
#define SLOW_POLL_PERIOD     5

/* ── 프레임 필드 인덱스 ──
 * DW3000/Makerfabs 예제 구조를 유지한다.
 * TX 길이는 2-byte FCS 포함 길이로 잡는 형태가 예제와 맞다.
 *  poll    : 12 bytes = [0..9 common] + [10..11 FCS 자리]
 *  response: 20 bytes = [0..9 common] + [10..17 timestamps] + [18..19 FCS 자리]
 */
#define ALL_MSG_SN_IDX          2
#define MSG_ANCHOR_ID_IDX       5
#define MSG_TAG_ID_IDX          6
#define MSG_MARKER0_IDX         7
#define MSG_MARKER1_IDX         8
#define MSG_TYPE_IDX            9
#define RESP_MSG_POLL_RX_TS_IDX 10
#define RESP_MSG_RESP_TX_TS_IDX 14
#define RESP_MSG_TS_LEN         4
#define POLL_MSG_LEN            12
#define RESP_MSG_LEN            20
#define RESP_MSG_MIN_READ_LEN   (RESP_MSG_RESP_TX_TS_IDX + RESP_MSG_TS_LEN)  /* 18 */
#define FRAME_MARKER0           'U'
#define FRAME_MARKER1           'W'
#define MSG_TYPE_POLL           0xE0
#define MSG_TYPE_RESP           0xE1

/* ═══════════════════════════════════════
 *  Phase 2-B: WiFi / MQTT 설정
 *  ← 현장 환경에 맞게 수정
 * ═══════════════════════════════════════ */
#define WIFI_SSID      "your_ssid"
#define WIFI_PASSWORD  "your_password"
#define MQTT_BROKER    "192.168.1.100"   /* 브로커 IP 또는 도메인 */
#define MQTT_PORT      1883
#define MQTT_USER      ""
#define MQTT_PASS      ""

#define RANGE_PUB_MS   1000
#define STATUS_PUB_MS  30000
#define MQTT_RETRY_MS  5000
/* ══════════════════════════════════════ */

static dwt_config_t config = {
  5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1,
  DWT_BR_6M8, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
  (129 + 8 - 8), DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0
};

static uint8_t tx_poll_msg[POLL_MSG_LEN] = {
  0x41, 0x88, 0, 0xCA, 0xDE,
  0,              /* [5] ANCHOR_ID */
  0,              /* [6] TAG_ID */
  FRAME_MARKER0,  /* [7] marker */
  FRAME_MARKER1,  /* [8] marker */
  MSG_TYPE_POLL,  /* [9] poll type */
  0, 0            /* [10-11] FCS 자리 */
};

static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[32];
static uint32_t status_reg = 0;

bool relayState    = false;
int  currentTagIdx = 0;

struct TagState {
  uint8_t id;
  bool    inDanger;
  int     outsideCount;
  int     noRangeCount;
  double  distBuf[DIST_FILTER_SIZE];
  int     distIdx;
  int     distCount;
  double  lastFiltered;
  int     absentCount;
  uint8_t pollSkip;
};

static TagState tagStates[8];

struct TagReport {
  uint8_t tagId;
  float   distRaw;       /* -1 = no range */
  float   distFiltered;
  bool    inDanger;
  bool    absent;
};

volatile bool      g_relayState         = false;
TagReport          g_tagReports[8]      = {};
uint32_t           g_reportTimestamp[8] = {};
portMUX_TYPE       g_reportMux          = portMUX_INITIALIZER_UNLOCKED;

static void processNoRange(int idx);

static void clearUwbStatus()
{
  dwt_write32bitreg(SYS_STATUS_ID,
                    SYS_STATUS_TXFRS_BIT_MASK |
                    SYS_STATUS_RXFCG_BIT_MASK |
                    SYS_STATUS_ALL_RX_TO |
                    SYS_STATUS_ALL_RX_ERR);
}

static bool waitForUwbStatus(uint32_t mask, uint32_t timeoutMs, uint32_t *outStatus)
{
  uint32_t start = millis();
  uint32_t st;
  do {
    st = dwt_read32bitreg(SYS_STATUS_ID);
    if (st & mask) {
      *outStatus = st;
      return true;
    }
    esp_task_wdt_reset();
  } while ((millis() - start) <= timeoutMs);

  *outStatus = 0;
  return false;
}

static void configureTaskWdt()
{
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  esp_err_t err = esp_task_wdt_reconfigure(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_init(&wdt_config);
  }
}

static void pushSample(TagState &t, double raw)
{
  t.distBuf[t.distIdx] = raw;
  t.distIdx = (t.distIdx + 1) % DIST_FILTER_SIZE;
  if (t.distCount < DIST_FILTER_SIZE) t.distCount++;
}

static double medianOfLast(TagState &t, int n)
{
  if (n > t.distCount) n = t.distCount;
  if (n <= 0) return -1.0;

  double tmp[DIST_FILTER_SIZE];
  for (int k = 0; k < n; k++) {
    int idx = (t.distIdx - 1 - k + DIST_FILTER_SIZE * 2) % DIST_FILTER_SIZE;
    tmp[k]  = t.distBuf[idx];
  }

  for (int i = 1; i < n; i++) {
    double key = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = key;
  }
  return tmp[n / 2];
}

static int relayOnLevel()  { return RELAY_ACTIVE_LOW ? LOW  : HIGH; }
static int relayOffLevel() { return RELAY_ACTIVE_LOW ? HIGH : LOW;  }

static void setRelay(bool on)
{
  relayState   = on;
  g_relayState = on;
  digitalWrite(RELAY_PIN, on ? relayOnLevel() : relayOffLevel());
}

static void relayInit()
{
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, relayOffLevel());
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

  if (rawDist <= 0.0 || rawDist > 30.0) {
    Serial.printf("[Tag#%d] invalid distance %.2fm -> treated as no range\n", t.id, rawDist);
    processNoRange(idx);
    return;
  }

  t.noRangeCount = 0;
  t.absentCount  = 0;
  t.pollSkip     = 0;
  pushSample(t, rawDist);

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

  t.absentCount++;
  bool nowAbsent = (t.absentCount >= SLOW_POLL_THRESHOLD);
  updateReport(idx, -1.0f, -1.0f, false, nowAbsent);

  if (nowAbsent) {
    t.pollSkip = SLOW_POLL_PERIOD - 1;
    if (t.absentCount == SLOW_POLL_THRESHOLD) {
      Serial.printf("[Tag#%d] 슬로 폴링 전환 (absent x%d) — 사이클 단축\n",
                    t.id, t.absentCount);
    }
  }
}

/* ════════════════════════════════════════════════════════
 *  MQTT publish 헬퍼 (Core 0 전용)
 * ════════════════════════════════════════════════════════ */

static void publishRange(PubSubClient &mqtt)
{
  TagReport localReports[8];
  uint32_t  localTs[8];

  portENTER_CRITICAL(&g_reportMux);
  for (int i = 0; i < NUM_TAGS; i++) {
    localReports[i] = g_tagReports[i];
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
  mqtt.publish(topic, payload, true);
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
  mqtt.publish(topic, payload, true);
}

static void taskNetwork(void *pvParameters)
{
  static WiFiClient   wifiClient;
  static PubSubClient mqtt(wifiClient);

  char lwt_topic[48];
  snprintf(lwt_topic, sizeof(lwt_topic), "uwb/anchor/%d/status", ANCHOR_ID);
  const char *lwt_payload = "{\"online\":false}";

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(5);

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
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[Net] WiFi 끊김 — 재연결 대기중");
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    if (!mqtt.connected()) {
      uint32_t now = millis();
      if (now - lastMqttRetry >= MQTT_RETRY_MS) {
        lastMqttRetry = now;

        uint64_t mac = ESP.getEfuseMac();
        char clientId[40];
        snprintf(clientId, sizeof(clientId), "anchor-%d-%04X", ANCHOR_ID, (uint16_t)(mac & 0xFFFF));

        bool ok = (strlen(MQTT_USER) > 0)
          ? mqtt.connect(clientId, MQTT_USER, MQTT_PASS, lwt_topic, 1, true, lwt_payload)
          : mqtt.connect(clientId, NULL, NULL,            lwt_topic, 1, true, lwt_payload);

        if (ok) {
          Serial.printf("[Net] MQTT 연결됨: %s:%d\n", MQTT_BROKER, MQTT_PORT);
          publishStatus(mqtt, true);
          publishRelay(mqtt, g_relayState);
          lastRelay     = g_relayState;
          lastStatusPub = millis();
        } else {
          Serial.printf("[Net] MQTT 연결 실패 (rc=%d) — %dms 후 재시도\n",
                        mqtt.state(), MQTT_RETRY_MS);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    mqtt.loop();

    uint32_t now = millis();
    bool curRelay = g_relayState;
    if (curRelay != lastRelay) {
      publishRelay(mqtt, curRelay);
      lastRelay = curRelay;
      Serial.printf("[Net] relay publish: %s\n", curRelay ? "ON" : "OFF");
    }

    if (now - lastRangePub >= RANGE_PUB_MS) {
      publishRange(mqtt);
      lastRangePub = now;
    }

    if (now - lastStatusPub >= STATUS_PUB_MS) {
      publishStatus(mqtt, true);
      lastStatusPub = now;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void taskUWB(void *pvParameters)
{
  esp_task_wdt_add(NULL);

  _fastSPI = SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0);
  spiBegin(PIN_IRQ, PIN_RST);
  spiSelect(PIN_SS);
  delay(2);

  dwt_softreset();
  delay(2);

  while (!dwt_checkidlerc()) { Serial.println("IDLE FAILED"); delay(100); }
  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) { Serial.println("INIT FAILED"); while (1) { delay(1000); } }

  dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
  if (dwt_configure(&config)) { Serial.println("CONFIG FAILED"); while (1) { delay(1000); } }

  dwt_configuretxrf(&txconfig_options);
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);
  dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
  dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

  clearUwbStatus();

  Serial.println("[UWB] task started on Core 1");
  Serial.printf("[UWB] SPI=%ldHz, rxAfterTx=%dus, rxTimeout=%dus\n",
                (long)SPI_HZ, POLL_TX_TO_RESP_RX_DLY_UUS, RESP_RX_TIMEOUT_UUS);

  for (;;) {
    esp_task_wdt_reset();

    if (tagStates[currentTagIdx].pollSkip > 0) {
      tagStates[currentTagIdx].pollSkip--;
      currentTagIdx = (currentTagIdx + 1) % NUM_TAGS;
      continue;
    }

    uint8_t targetTagId = TAG_IDS[currentTagIdx];

    tx_poll_msg[ALL_MSG_SN_IDX]    = frame_seq_nb;
    tx_poll_msg[MSG_ANCHOR_ID_IDX] = ANCHOR_ID;
    tx_poll_msg[MSG_TAG_ID_IDX]    = targetTagId;
    tx_poll_msg[MSG_MARKER0_IDX]   = FRAME_MARKER0;
    tx_poll_msg[MSG_MARKER1_IDX]   = FRAME_MARKER1;
    tx_poll_msg[MSG_TYPE_IDX]      = MSG_TYPE_POLL;

    clearUwbStatus();
    dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
    dwt_writetxfctrl(sizeof(tx_poll_msg), 0, 1);

    int txRet = dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
    if (txRet != DWT_SUCCESS) {
      Serial.printf("[Tag#%d] poll TX failed -> no range\n", targetTagId);
      clearUwbStatus();
      processNoRange(currentTagIdx);
      currentTagIdx = (currentTagIdx + 1) % NUM_TAGS;
      vTaskDelay(pdMS_TO_TICKS(RNG_DELAY_MS));
      continue;
    }

    waitForUwbStatus(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR,
                     50, &status_reg);

    frame_seq_nb++;

    bool gotValidResponse = false;

    if (status_reg & SYS_STATUS_RXFCG_BIT_MASK) {
      dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
      uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;

      if (frame_len <= sizeof(rx_buffer) && frame_len >= RESP_MSG_MIN_READ_LEN) {
        memset(rx_buffer, 0, sizeof(rx_buffer));
        dwt_readrxdata(rx_buffer, frame_len, 0);
        rx_buffer[ALL_MSG_SN_IDX] = 0;

        bool headerOk =
          rx_buffer[0]                 == 0x41 &&
          rx_buffer[1]                 == 0x88 &&
          rx_buffer[3]                 == 0xCA &&
          rx_buffer[4]                 == 0xDE &&
          rx_buffer[MSG_ANCHOR_ID_IDX] == ANCHOR_ID &&
          rx_buffer[MSG_TAG_ID_IDX]    == targetTagId &&
          rx_buffer[MSG_MARKER0_IDX]   == FRAME_MARKER0 &&
          rx_buffer[MSG_MARKER1_IDX]   == FRAME_MARKER1 &&
          rx_buffer[MSG_TYPE_IDX]      == MSG_TYPE_RESP;

        if (headerOk) {
          gotValidResponse = true;

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
        } else {
          Serial.printf("[Tag#%d] bad response frame len=%lu aid=%u tid=%u type=0x%02X\n",
                        targetTagId, (unsigned long)frame_len,
                        rx_buffer[MSG_ANCHOR_ID_IDX], rx_buffer[MSG_TAG_ID_IDX], rx_buffer[MSG_TYPE_IDX]);
        }
      } else {
        Serial.printf("[Tag#%d] bad response length=%lu\n", targetTagId, (unsigned long)frame_len);
      }
    } else {
      Serial.printf("[Tag#%d] no range status=0x%08lX relay:%s\n",
                    targetTagId, (unsigned long)status_reg, relayState ? "ON" : "OFF");
    }

    if (!gotValidResponse) {
      clearUwbStatus();
      processNoRange(currentTagIdx);
    }

    currentTagIdx = (currentTagIdx + 1) % NUM_TAGS;
    vTaskDelay(pdMS_TO_TICKS(RNG_DELAY_MS));
  }
}

void setup()
{
  UART_init();
  Serial.begin(115200);

  relayInit();

  for (int i = 0; i < NUM_TAGS; i++) {
    tagStates[i] = {TAG_IDS[i], false, 0, 0, {0}, 0, 0, 0.0, 0, 0};
    updateReport(i, -1.0f, -1.0f, false, false);
  }

  configureTaskWdt();

  Serial.println();
  Serial.println("=================================");
  Serial.printf( "UWB ANCHOR #%d  (FIXED)\n", ANCHOR_ID);
  Serial.printf( "Tags     : %d\n", NUM_TAGS);
  for (int i = 0; i < NUM_TAGS; i++) Serial.printf("  - Tag#%d\n", TAG_IDS[i]);
  Serial.printf( "Safety   : %.1fm / %.1fm (enter/exit)\n", ENTER_DISTANCE_M, EXIT_DISTANCE_M);
  Serial.printf( "Filter   : median asym (enter=%d, exit=%d)\n", ENTER_FILTER_SIZE, DIST_FILTER_SIZE);
  Serial.printf( "Frame    : poll=%d resp=%d typeIdx=%d\n", POLL_MSG_LEN, RESP_MSG_LEN, MSG_TYPE_IDX);
  Serial.printf( "MQTT     : %s:%d\n", MQTT_BROKER, MQTT_PORT);
  Serial.println("=================================");

  xTaskCreatePinnedToCore(
    taskUWB, "UWB_Safety",
    8192, NULL,
    2,
    NULL, 1
  );

  xTaskCreatePinnedToCore(
    taskNetwork, "Network",
    8192,
    NULL,
    1,
    NULL, 0
  );
}

void loop()
{
  vTaskDelete(NULL);
}

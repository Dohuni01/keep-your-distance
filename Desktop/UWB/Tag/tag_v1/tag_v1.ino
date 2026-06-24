#include "dw3000.h"
#include "SPI.h"
#include "esp_task_wdt.h"
#include "esp_err.h"
#include <string.h>

extern SPISettings _fastSPI;
extern dwt_txconfig_t txconfig_options;

/* Makerfabs ESP32 UWB DW3000 기본 핀 */
#define PIN_RST 27
#define PIN_IRQ 34
#define PIN_SS  4

/* ── 디바이스 설정 (태그마다 MY_TAG_ID 변경: 1, 2, 3, 4 ...) ── */
#define MY_TAG_ID 4

/* ── DW3000 설정 ──
 * no range 디버깅용으로 Makerfabs 예제와 같은 SPI 7MHz + softreset +
 * 12-byte poll / 20-byte response 구조를 사용한다. */
#define SPI_HZ                         7000000L
#define TX_ANT_DLY                     16385
#define RX_ANT_DLY                     16385
#define POLL_RX_TO_RESP_TX_DLY_UUS     1500
#define WDT_TIMEOUT_S                  10

/* RX 타임아웃: 너무 크게 잡으면 일부 라이브러리/레지스터 폭에서 애매할 수 있어 60ms로 제한.
 * 앵커가 없어도 주기적으로 loop가 풀려 WDT가 리셋된다. */
#define RX_TIMEOUT_UUS                 60000

/* ── 프레임 필드 인덱스 (Anchor와 동일) ──
 * poll    : 12 bytes = [0..9 common] + [10..11 FCS 자리]
 * response: 20 bytes = [0..9 common] + [10..17 timestamps] + [18..19 FCS 자리]
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
#define POLL_MSG_MIN_READ_LEN   10
#define RESP_MSG_LEN            20
#define FRAME_MARKER0           'U'
#define FRAME_MARKER1           'W'
#define MSG_TYPE_POLL           0xE0
#define MSG_TYPE_RESP           0xE1

static dwt_config_t config = {
  5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1,
  DWT_BR_6M8, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
  (129 + 8 - 8), DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0
};

/* 응답 프레임 — ANCHOR_ID, SEQ는 poll 수신 후 채움 */
static uint8_t tx_resp_msg[RESP_MSG_LEN] = {
  0x41, 0x88, 0, 0xCA, 0xDE,
  0,              /* [5]  ANCHOR_ID  (poll에서 복사) */
  MY_TAG_ID,      /* [6]  MY_TAG_ID */
  FRAME_MARKER0,  /* [7]  marker */
  FRAME_MARKER1,  /* [8]  marker */
  MSG_TYPE_RESP,  /* [9]  response type */
  0, 0, 0, 0,     /* [10-13] poll_rx_ts */
  0, 0, 0, 0,     /* [14-17] resp_tx_ts */
  0, 0            /* [18-19] FCS 자리 */
};

static uint8_t  frame_seq_nb = 0;
static uint8_t  rx_buffer[32];
static uint32_t status_reg = 0;

static uint64_t poll_rx_ts;
static uint64_t resp_tx_ts;

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

void setup()
{
  UART_init();
  Serial.begin(115200);

  configureTaskWdt();
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
  dwt_setrxtimeout(RX_TIMEOUT_UUS);
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

  clearUwbStatus();

  Serial.println();
  Serial.println("=================================");
  Serial.printf( "UWB TAG #%d READY (FIXED)\n", MY_TAG_ID);
  Serial.printf( "SPI      : %ld Hz\n", (long)SPI_HZ);
  Serial.printf( "Frame    : poll=%d resp=%d typeIdx=%d\n", POLL_MSG_LEN, RESP_MSG_LEN, MSG_TYPE_IDX);
  Serial.printf( "RespDly  : %d uus\n", POLL_RX_TO_RESP_TX_DLY_UUS);
  Serial.println("Waiting for anchor polls...");
  Serial.println("=================================");
}

void loop()
{
  esp_task_wdt_reset();

  clearUwbStatus();
  dwt_rxenable(DWT_START_RX_IMMEDIATE);

  waitForUwbStatus(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR,
                   RX_TIMEOUT_UUS / 1000 + 20, &status_reg);

  if (status_reg & SYS_STATUS_RXFCG_BIT_MASK) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;

    if (frame_len <= sizeof(rx_buffer) && frame_len >= POLL_MSG_MIN_READ_LEN) {
      memset(rx_buffer, 0, sizeof(rx_buffer));
      dwt_readrxdata(rx_buffer, frame_len, 0);
      rx_buffer[ALL_MSG_SN_IDX] = 0;

      bool pollForMe =
        rx_buffer[0]                 == 0x41 &&
        rx_buffer[1]                 == 0x88 &&
        rx_buffer[3]                 == 0xCA &&
        rx_buffer[4]                 == 0xDE &&
        rx_buffer[MSG_TAG_ID_IDX]    == MY_TAG_ID &&
        rx_buffer[MSG_MARKER0_IDX]   == FRAME_MARKER0 &&
        rx_buffer[MSG_MARKER1_IDX]   == FRAME_MARKER1 &&
        rx_buffer[MSG_TYPE_IDX]      == MSG_TYPE_POLL;

      if (pollForMe) {
        uint8_t senderAnchorId = rx_buffer[MSG_ANCHOR_ID_IDX];

        poll_rx_ts = get_rx_timestamp_u64();
        uint32_t resp_tx_time = (poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;

        dwt_setdelayedtrxtime(resp_tx_time);
        resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

        tx_resp_msg[ALL_MSG_SN_IDX]    = frame_seq_nb;
        tx_resp_msg[MSG_ANCHOR_ID_IDX] = senderAnchorId;
        tx_resp_msg[MSG_TAG_ID_IDX]    = MY_TAG_ID;
        tx_resp_msg[MSG_MARKER0_IDX]   = FRAME_MARKER0;
        tx_resp_msg[MSG_MARKER1_IDX]   = FRAME_MARKER1;
        tx_resp_msg[MSG_TYPE_IDX]      = MSG_TYPE_RESP;

        resp_msg_set_ts(&tx_resp_msg[RESP_MSG_POLL_RX_TS_IDX], poll_rx_ts);
        resp_msg_set_ts(&tx_resp_msg[RESP_MSG_RESP_TX_TS_IDX], resp_tx_ts);

        clearUwbStatus();
        dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
        dwt_writetxfctrl(sizeof(tx_resp_msg), 0, 1);

        int ret = dwt_starttx(DWT_START_TX_DELAYED);
        if (ret == DWT_SUCCESS) {
          uint32_t txStatus;
          waitForUwbStatus(SYS_STATUS_TXFRS_BIT_MASK, 20, &txStatus);
          dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
          frame_seq_nb++;
          Serial.printf("[Anchor#%d -> Tag#%d] Responded\n", senderAnchorId, MY_TAG_ID);
        } else {
          Serial.printf("[Tag#%d] TX LATE — increase POLL_RX_TO_RESP_TX_DLY_UUS\n", MY_TAG_ID);
        }
      }
      /* 다른 태그를 향한 poll은 조용히 무시 */
    } else {
      Serial.printf("[Tag#%d] bad poll length=%lu\n", MY_TAG_ID, (unsigned long)frame_len);
    }
  } else if (status_reg & SYS_STATUS_ALL_RX_ERR) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
  } else {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO);
  }

  delay(1);
}

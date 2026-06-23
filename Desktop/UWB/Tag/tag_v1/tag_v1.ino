#include "dw3000.h"
#include "SPI.h"
#include "esp_task_wdt.h"

extern SPISettings _fastSPI;
extern dwt_txconfig_t txconfig_options;

/*
   Makerfabs ESP32 UWB DW3000 기본 핀
*/
#define PIN_RST 27
#define PIN_IRQ 34
#define PIN_SS  4

/* ── 디바이스 설정 (태그마다 MY_TAG_ID 변경: 1, 2, 3, 4 ...) ── */
#define MY_TAG_ID 1

#define TX_ANT_DLY              16385
#define RX_ANT_DLY              16385
#define POLL_RX_TO_RESP_TX_DLY_UUS 800
#define WDT_TIMEOUT_S           10

/* ── RX 타임아웃 (리뷰 C2) ──
 * 단위 ≈ 1.026us. 0 = 무한 대기(기존 버그: 앵커 부재 시 spin이 안 풀려
 * WDT가 리셋되지 못하고 10초마다 리부트). 타임아웃을 두면 수신기가 주기적으로
 * 풀려 loop()가 재진입하고 WDT가 리셋된다 → 앵커 부재를 정상 idle로 처리.
 * 150000 ≈ 150ms. 풀리면 즉시 재수신하므로 deaf 구간은 무시할 수준. */
#define RX_TIMEOUT_UUS          150000

/* ── 프레임 필드 인덱스 (Anchor와 동일) ── */
#define ALL_MSG_SN_IDX          2
#define MSG_ANCHOR_ID_IDX       5
#define MSG_TAG_ID_IDX          6
#define MSG_TYPE_IDX            7
#define RESP_MSG_POLL_RX_TS_IDX 10
#define RESP_MSG_RESP_TX_TS_IDX 14

static dwt_config_t config = {
  5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1,
  DWT_BR_6M8, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
  (129 + 8 - 8), DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0
};

/* 응답 프레임 — ANCHOR_ID, SEQ는 poll 수신 후 채움 */
static uint8_t tx_resp_msg[] = {
  0x41, 0x88, 0, 0xCA, 0xDE,
  0,     /* [5]  ANCHOR_ID  (poll에서 복사) */
  0,     /* [6]  MY_TAG_ID  (항상 MY_TAG_ID) */
  0xE1,  /* [7]  response type */
  0, 0,  /* [8-9]  패딩 */
  0, 0, 0, 0,  /* [10-13] poll_rx_ts */
  0, 0, 0, 0   /* [14-17] resp_tx_ts */
};

static uint8_t  frame_seq_nb = 0;
static uint8_t  rx_buffer[20];
static uint32_t status_reg = 0;

static uint64_t poll_rx_ts;
static uint64_t resp_tx_ts;

void setup()
{
  UART_init();
  Serial.begin(115200);

  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  _fastSPI = SPISettings(16000000L, MSBFIRST, SPI_MODE0);
  spiBegin(PIN_IRQ, PIN_RST);
  spiSelect(PIN_SS);
  delay(1000);

  while (!dwt_checkidlerc()) { Serial.println("IDLE FAILED"); delay(100); }
  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) { Serial.println("INIT FAILED"); while (1); }

  dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
  if (dwt_configure(&config)) { Serial.println("CONFIG FAILED"); while (1); }

  dwt_configuretxrf(&txconfig_options);
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);
  dwt_setrxtimeout(RX_TIMEOUT_UUS);  /* C2: 무한 spin 방지 → 앵커 부재 시 리부트 안 함 */
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

  Serial.println();
  Serial.println("=================================");
  Serial.printf( "UWB TAG #%d READY\n", MY_TAG_ID);
  Serial.println("Waiting for anchor polls...");
  Serial.println("=================================");
}

void loop()
{
  esp_task_wdt_reset();

  dwt_rxenable(DWT_START_RX_IMMEDIATE);

  /* RXFCG(수신) | RX_TO(타임아웃) | RX_ERR(에러) 중 하나가 뜰 때까지 대기.
   * 타임아웃이 있으므로 앵커가 없어도 spin이 풀려 다음 loop()에서 WDT가 리셋된다. */
  while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
           (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR))) {}

  if (status_reg & SYS_STATUS_RXFCG_BIT_MASK) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;

    if (frame_len <= sizeof(rx_buffer)) {
      dwt_readrxdata(rx_buffer, frame_len, 0);
      rx_buffer[ALL_MSG_SN_IDX] = 0;

      /* 자신에게 온 poll인지 확인 — Anchor ID는 상관없이 수락 (향후 다중 앵커 대응) */
      if (rx_buffer[MSG_TYPE_IDX]   == 0xE0 &&
          rx_buffer[MSG_TAG_ID_IDX] == MY_TAG_ID) {

        uint8_t senderAnchorId = rx_buffer[MSG_ANCHOR_ID_IDX];

        poll_rx_ts = get_rx_timestamp_u64();
        uint32_t resp_tx_time = (poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;

        dwt_setdelayedtrxtime(resp_tx_time);
        resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

        /* 응답 프레임 구성 */
        tx_resp_msg[ALL_MSG_SN_IDX]    = frame_seq_nb;
        tx_resp_msg[MSG_ANCHOR_ID_IDX] = senderAnchorId;
        tx_resp_msg[MSG_TAG_ID_IDX]    = MY_TAG_ID;

        resp_msg_set_ts(&tx_resp_msg[RESP_MSG_POLL_RX_TS_IDX], poll_rx_ts);
        resp_msg_set_ts(&tx_resp_msg[RESP_MSG_RESP_TX_TS_IDX], resp_tx_ts);

        dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
        dwt_writetxfctrl(sizeof(tx_resp_msg), 0, 1);

        if (dwt_starttx(DWT_START_TX_DELAYED) == DWT_SUCCESS) {
          while (!(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS_BIT_MASK)) {}
          dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
          frame_seq_nb++;
          Serial.printf("[Anchor#%d -> Tag#%d] Responded\n", senderAnchorId, MY_TAG_ID);
        }
        /* 다른 태그를 향한 poll은 무시하고 다음 수신 대기 */
      }
    }
  } else {
    /* 타임아웃 또는 에러 → 상태 클리어, 다음 loop()에서 재수신 (리부트 없음) */
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
  }
}

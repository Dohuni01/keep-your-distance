/* ════════════════════════════════════════════════════════════════
 *  테스트 A — 다중 앵커 충돌 곡선 측정 도구
 *  (Multi-Anchor Channel Collision / PER measurement)
 *
 *  목적:
 *    단일 RF 채널에서 앵커가 1대일 때 vs 2대가 동시에 폴링할 때
 *    "no-range 발생률(PER)"이 얼마나 오르는지 측정한다.
 *    → 100대 확장이 물리적으로 가능한지 판단하는 1차 데이터.
 *
 *  프로덕션(ancher_v1.ino)과 동일하게 유지한 것:
 *    - DW3000 config (채널/프리앰블/데이터레이트/STS)
 *    - 프레임 포맷, ANCHOR_ID/TAG_ID/MSG_TYPE 필드
 *    - SS-TWR 교환 시퀀스 + 안테나 딜레이 + RX 타임아웃
 *    → 측정 결과가 실제 시스템에 그대로 전이된다.
 *
 *  프로덕션과 다르게 한 것 (측정 순수성):
 *    - WiFi / MQTT / 릴레이 제거 (RF만 깨끗하게)
 *    - FreeRTOS 이중 코어 제거 (단일 loop, 타이밍 단순화)
 *    - 성공/실패 카운터 + 주기적 통계 출력 추가
 *
 *  태그 펌웨어는 수정 불필요:
 *    tag_v1.ino는 어느 ANCHOR_ID의 poll이든 수락하므로,
 *    앵커 1·2가 같은 태그를 폴링하면 태그가 각각에 응답한다.
 * ════════════════════════════════════════════════════════════════ */

#include "dw3000.h"
#include "SPI.h"

extern SPISettings _fastSPI;
extern dwt_txconfig_t txconfig_options;

/* ── 핀 (프로덕션과 동일) ── */
#define PIN_RST 27
#define PIN_IRQ 34
#define PIN_SS  4

/* ════════════════════════════════════════════════════════════════
 *  ▼▼▼ 측정 파라미터 — 보드/실험마다 여기만 바꿔서 재플래시 ▼▼▼
 * ════════════════════════════════════════════════════════════════ */

/* 이 보드의 앵커 ID. 보드 #1 → 1, 보드 #2 → 2 로 각각 플래시.
 * 두 앵커가 같은 TARGET_TAG_ID를 폴링하게 해서 충돌을 유발한다. */
#define TEST_ANCHOR_ID    1

/* 두 앵커가 동시에 노리는 타깃 태그. 물리 태그 1개를 MY_TAG_ID=1로 켠다. */
#define TARGET_TAG_ID     1

/* RF 채널. 테스트 A는 둘 다 5로. (테스트 E에서 한쪽을 9로 바꿔 분리 검증) */
#define TEST_CHANNEL      5

/* 폴링 부하(load). 작을수록 에어타임을 더 많이 먹어 충돌이 늘어난다.
 * 부하 스윕(아래 README 참고): 150 → 50 → 20 → 10 → 5 → 2 → 0
 * 각 값으로 1앵커/2앵커를 측정해 PER 곡선을 그린다. */
#define POLL_DELAY_MS     150

/* 통계 출력 주기 (폴링 횟수 기준). */
#define STATS_WINDOW      200

/* ════════════════════════════════════════════════════════════════
 *  ▲▲▲ 여기까지 ▲▲▲
 * ════════════════════════════════════════════════════════════════ */

/* ── DW3000 타이밍 (프로덕션과 동일) ── */
#define TX_ANT_DLY                 16385
#define RX_ANT_DLY                 16385
#define POLL_TX_TO_RESP_RX_DLY_UUS 240
#define RESP_RX_TIMEOUT_UUS        3000

/* ── 프레임 필드 인덱스 (프로덕션과 동일) ── */
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

static uint8_t tx_poll_msg[] = {
  0x41, 0x88, 0, 0xCA, 0xDE,
  0, 0, 0xE0, 0, 0
};

static uint8_t  frame_seq_nb = 0;
static uint8_t  rx_buffer[20];
static uint32_t status_reg = 0;

/* ── 통계 누적기 ── */
static uint32_t winPolls   = 0;   /* 이번 윈도우 폴링 수 */
static uint32_t winOk      = 0;   /* 이번 윈도우 유효 거리 성공 수 */
static uint32_t winWrong   = 0;   /* CRC는 통과했으나 내 응답이 아님(타 앵커/오태그) */
static double   winDistSum = 0.0; /* 성공 거리 합 (평균 산출용) */
static uint32_t totalPolls = 0;
static uint32_t totalOk    = 0;

static void printStats()
{
  uint32_t fail = winPolls - winOk;            /* no-range + 잘못된 프레임 모두 실패 */
  double per   = winPolls ? (100.0 * fail / winPolls) : 0.0;
  double dAvg  = winOk    ? (winDistSum / winOk)       : 0.0;

  /* 사람이 읽는 줄 */
  Serial.printf(
    "[CH%d A%d delay=%dms] polls=%lu ok=%lu fail=%lu wrong=%lu  PER=%.1f%%  dist_avg=%.2fm\n",
    TEST_CHANNEL, TEST_ANCHOR_ID, POLL_DELAY_MS,
    (unsigned long)winPolls, (unsigned long)winOk,
    (unsigned long)fail, (unsigned long)winWrong, per, dAvg);

  /* CSV 줄 — 시리얼 로그를 엑셀/파이썬으로 바로 분석.
   * 헤더: ch,anchor,delay_ms,polls,ok,fail,wrong,per_pct,dist_avg_m */
  Serial.printf("CSV,%d,%d,%d,%lu,%lu,%lu,%lu,%.1f,%.2f\n",
    TEST_CHANNEL, TEST_ANCHOR_ID, POLL_DELAY_MS,
    (unsigned long)winPolls, (unsigned long)winOk,
    (unsigned long)fail, (unsigned long)winWrong, per, dAvg);

  winPolls = winOk = winWrong = 0;
  winDistSum = 0.0;
}

void setup()
{
  UART_init();
  Serial.begin(115200);

  _fastSPI = SPISettings(16000000L, MSBFIRST, SPI_MODE0);
  spiBegin(PIN_IRQ, PIN_RST);
  spiSelect(PIN_SS);
  delay(2);

  while (!dwt_checkidlerc()) { Serial.println("IDLE FAILED"); delay(100); }
  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) { Serial.println("INIT FAILED"); while (1); }

  config.chan = TEST_CHANNEL;          /* 채널만 파라미터로 교체 (테스트 E 대비) */

  dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
  if (dwt_configure(&config)) { Serial.println("CONFIG FAILED"); while (1); }

  dwt_configuretxrf(&txconfig_options);
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);
  dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
  dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("  TEST A — multi-anchor collision / PER meter");
  Serial.printf ("  ANCHOR_ID=%d  TARGET_TAG=%d  CH=%d  delay=%dms\n",
                 TEST_ANCHOR_ID, TARGET_TAG_ID, TEST_CHANNEL, POLL_DELAY_MS);
  Serial.printf ("  stats every %d polls\n", STATS_WINDOW);
  Serial.println("  CSV cols: ch,anchor,delay_ms,polls,ok,fail,wrong,per_pct,dist_avg_m");
  Serial.println("==================================================");
}

void loop()
{
  /* ── 단일 SS-TWR 교환 (프로덕션 taskUWB와 동일 시퀀스) ── */
  tx_poll_msg[ALL_MSG_SN_IDX]    = frame_seq_nb;
  tx_poll_msg[MSG_ANCHOR_ID_IDX] = TEST_ANCHOR_ID;
  tx_poll_msg[MSG_TAG_ID_IDX]    = TARGET_TAG_ID;

  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
  dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
  dwt_writetxfctrl(sizeof(tx_poll_msg), 0, 1);
  dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

  while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
           (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR))) {}

  frame_seq_nb++;
  winPolls++;
  totalPolls++;

  if (status_reg & SYS_STATUS_RXFCG_BIT_MASK) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;

    if (frame_len <= sizeof(rx_buffer)) {
      dwt_readrxdata(rx_buffer, frame_len, 0);
      rx_buffer[ALL_MSG_SN_IDX] = 0;

      if (rx_buffer[MSG_ANCHOR_ID_IDX] == TEST_ANCHOR_ID &&
          rx_buffer[MSG_TAG_ID_IDX]    == TARGET_TAG_ID  &&
          rx_buffer[MSG_TYPE_IDX]      == 0xE1) {
        /* 내 타깃 태그가 나에게 보낸 유효 응답 → 거리 계산 */
        uint32_t poll_tx_ts = dwt_readtxtimestamplo32();
        uint32_t resp_rx_ts = dwt_readrxtimestamplo32();
        float    clockOffsetRatio = ((float)dwt_readclockoffset()) / (uint32_t)(1 << 26);

        uint32_t poll_rx_ts, resp_tx_ts_val;
        resp_msg_get_ts(&rx_buffer[RESP_MSG_POLL_RX_TS_IDX], &poll_rx_ts);
        resp_msg_get_ts(&rx_buffer[RESP_MSG_RESP_TX_TS_IDX], &resp_tx_ts_val);

        int32_t rtd_init = resp_rx_ts     - poll_tx_ts;
        int32_t rtd_resp = resp_tx_ts_val - poll_rx_ts;
        double  tof      = ((rtd_init - rtd_resp * (1 - clockOffsetRatio)) / 2.0) * DWT_TIME_UNITS;
        double  dist     = tof * SPEED_OF_LIGHT;

        if (dist > 0.0 && dist < 100.0) {
          winOk++;
          totalOk++;
          winDistSum += dist;
        } else {
          /* 타임스탬프는 받았으나 비현실적 거리 → 실패로 집계 */
        }
      } else {
        /* CRC는 통과했지만 타 앵커 응답이거나 다른 태그 → 내 교환 실패 */
        winWrong++;
      }
    }
  } else {
    /* RX 타임아웃 또는 에러 = no-range (충돌의 주된 형태) */
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
  }

  if (winPolls >= STATS_WINDOW) printStats();

  if (POLL_DELAY_MS > 0) delay(POLL_DELAY_MS);
}

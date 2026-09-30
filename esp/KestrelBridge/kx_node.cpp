// Kestrel Phase 2 mailbox node, ESP32-S3 side.
//
// Tasks:
//   kx_mbox (core 1, prio 3): UART1 <-> RA4 at 1 Mbaud. Session handshake, PING/TIME_SYNC, decodes
//            RA4 frames, forwards them to the net task. Subscribed to the task watchdog (2 s, panic).
//   kx_net  (core 0, prio 2): Wi-Fi SoftAP (WPA2) + UDP. Port 47000: text control and telemetry
//            forwarding to the subscribed peer. Port 47002: flood sink (bytes counted, discarded).
// Fault modes (UDP "fault ..." commands) implement Phase 2 cases F2-F11; see docs/PHASE2_FAULT_MODEL.md.
#include "kx_node.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

extern "C" {
#include "xlink.h"
}
#include "kx_secrets.h"  // KX_AP_SSID, KX_AP_PASS (generated, gitignored)

#define KX_UART_RX 6
#define KX_UART_TX 5
#define KX_BAUD 1000000
#define KX_CTRL_PORT 47000
#define KX_SINK_PORT 47002
#define KX_HANDSHAKE_RETRY_MS 300
#define KX_PING_MS 100
#define KX_TSYNC_MS 1000
#define KX_WDT_S 2
#define KX_DESYNC_MS 500

#define RTC_MAGIC 0x4B58524Du  // "KXRM"
#define REPLAY_SLOTS 16

typedef struct {
  uint32_t magic;
  uint32_t boot_count;
  uint32_t reboot_loop_left;
  uint32_t reboot_loop_delay_ms;
  uint32_t replay_pending;
  uint32_t replay_n;
  uint16_t replay_len[REPLAY_SLOTS];
  uint8_t replay[REPLAY_SLOTS][32];
} rtc_state_t;
RTC_NOINIT_ATTR static rtc_state_t s_rtc;

typedef struct {
  uint16_t len;
  uint8_t b[XL_OVERHEAD + 160];
} fwd_t;

static QueueHandle_t s_fwd;  // RA4 frames -> net task
static volatile uint32_t* s_usb_baud;

// ---- state shared between tasks (single writer per field) ----
static volatile uint32_t s_ra4_epoch, s_session, s_established;
static volatile uint32_t st_rx_frames, st_rx_crc, st_rx_len, st_rx_ver, st_rx_skipped, st_hb, st_pong;
static volatile uint32_t st_tx_frames, st_tx_bytes, st_handshakes, st_rtt_min = 0xFFFFFFFF, st_rtt_max, st_rtt_n;
static volatile uint64_t st_rtt_sum;
static volatile uint32_t st_fwd_drop, st_sink_pkts, st_ctrl_pkts, st_config_acks, st_config_nacks;
static volatile uint64_t st_sink_bytes;
static volatile uint32_t st_ra4_ctl_jobs, st_ra4_ctl_miss, st_ra4_degraded, st_ra4_boot;
static volatile uint32_t st_bad_sent[8];  // F4 classes sent
static volatile uint32_t st_valid_sent;   // valid in-session frames sent after HELLO (RA4 should accept)
static volatile uint32_t st_uart_rx_bytes;
static volatile int st_wdt_rc = -1;

// ---- fault controls (written by net task, read by mbox task) ----
static volatile uint32_t f_until_ms[16];
enum { F_MALFORMED = 4, F_FLOOD = 5, F_NOREAD = 9, F_NOTX = 10, F_HANG = 12 };
static volatile uint32_t f_malformed_rate = 200;
static volatile uint32_t s_cfg_param, s_cfg_value, s_cfg_pending;

static bool active(int f) { return (int32_t)(millis() - f_until_ms[f]) < 0; }

// ---------------- mailbox task ----------------
static uint32_t s_tx_seq;
static uint32_t s_hs_sent_ms;
static uint32_t s_acked_seq, s_acked_ms;
static volatile uint32_t st_resyncs;

static void record_replay(const uint8_t* b, size_t n)
{
  if (n > 32) return;
  uint32_t i = s_rtc.replay_n % REPLAY_SLOTS;
  memcpy(s_rtc.replay[i], b, n);
  s_rtc.replay_len[i] = (uint16_t)n;
  s_rtc.replay_n++;
}

static void tx_raw(const uint8_t* b, size_t n)
{
  if (active(F_NOTX)) return;
  Serial1.write(b, n);
  st_tx_bytes += n;
}

static void tx_frame(uint8_t type, const uint8_t* p, uint16_t len, bool count_valid = true)
{
  if (active(F_NOTX)) return;  // a frame not sent must not consume a sequence number
  uint8_t b[XL_OVERHEAD + 32];
  size_t n = xl_encode(b, sizeof(b), type, (uint16_t)s_session, s_tx_seq + 1, p, len);
  if (n == 0) return;
  s_tx_seq++;
  tx_raw(b, n);
  st_tx_frames++;
  if (count_valid && s_established) st_valid_sent++;
  record_replay(b, n);
}

static void start_handshake(void)
{
  uint16_t sid;
  do {
    sid = (uint16_t)esp_random();
  } while (sid == 0);
  s_session = sid;
  s_established = 0;
  s_tx_seq = 0;
  s_acked_seq = 0;
  s_acked_ms = millis();
  uint8_t p[8];
  xl_put32(p, s_ra4_epoch);
  xl_put32(&p[4], s_rtc.boot_count);
  tx_frame(XL_HELLO, p, 8, false);
  s_hs_sent_ms = millis();
  st_handshakes++;
}

static void send_malformed(void)
{
  static uint32_t k;
  uint8_t b[XL_OVERHEAD + 16];
  uint8_t p[8] = {0};
  uint32_t cls = k++ % 7;
  size_t n;
  switch (cls) {
    case 0:  // random garbage
      n = 20;
      for (size_t i = 0; i < n; i++) b[i] = (uint8_t)esp_random();
      break;
    case 1:  // valid frame, one bit flipped
      n = xl_encode(b, sizeof(b), XL_PING, (uint16_t)s_session, s_tx_seq + 1, p, 4);
      b[4 + (esp_random() % (n - 4))] ^= (uint8_t)(1u << (esp_random() & 7));
      break;
    case 2:  // oversize length field
      n = xl_encode(b, sizeof(b), XL_PING, (uint16_t)s_session, s_tx_seq + 1, p, 4);
      xl_put16(&b[10], 0xFFFF);
      break;
    case 3:  // bad version
      n = xl_encode(b, sizeof(b), XL_PING, (uint16_t)s_session, s_tx_seq + 1, p, 4);
      b[2] = 9;
      break;
    case 4:  // unknown type, valid CRC
      n = xl_encode(b, sizeof(b), 0x42, (uint16_t)s_session, s_tx_seq + 1, p, 4);
      break;
    case 5:  // known type, wrong payload length, valid CRC
      n = xl_encode(b, sizeof(b), XL_PING, (uint16_t)s_session, s_tx_seq + 1, p, 7);
      break;
    default:  // replay: in-session, old sequence number, valid CRC
      n = xl_encode(b, sizeof(b), XL_PING, (uint16_t)s_session, s_tx_seq > 1 ? s_tx_seq - 1 : 1, p, 4);
      break;
  }
  st_bad_sent[cls]++;
  tx_raw(b, n);
}

static void on_ra4_frame(const xl_frame_t* f)
{
  st_rx_frames++;
  uint32_t now = millis();
  if (f->type == XL_HEARTBEAT && f->len >= 23) {
    st_hb++;
    uint32_t ep = xl_get32(f->payload);
    uint16_t ses = xl_get16(&f->payload[8]);
    st_ra4_ctl_jobs = xl_get32(&f->payload[14]);
    st_ra4_ctl_miss = xl_get32(&f->payload[18]);
    st_ra4_degraded = f->payload[22];
    if (ep != s_ra4_epoch) {  // first contact or RA4 reboot: new session
      s_ra4_epoch = ep;
      start_handshake();
    } else if (ses == (uint16_t)s_session) {
      uint32_t acked = xl_get32(&f->payload[10]);  // last ESP sequence the RA4 accepted
      if (acked != s_acked_seq) {
        s_acked_seq = acked;
        s_acked_ms = now;
      } else if (s_established && s_tx_seq > acked && (now - s_acked_ms) > KX_DESYNC_MS) {
        st_resyncs++;  // RA4 refuses everything we send: start a clean session
        start_handshake();
        return;
      }
      if (!s_established) {
        s_established = 1;
        uint8_t m[4];
        xl_put32(m, 0xFFFFFFFFu);
        tx_frame(XL_TELEM_CTRL, m, 4);
      }
    }
  } else if (f->type == XL_PONG && f->len >= 8) {
    st_pong++;
    uint32_t rtt = (uint32_t)micros() - xl_get32(f->payload);
    if (rtt < st_rtt_min) st_rtt_min = rtt;
    if (rtt > st_rtt_max) st_rtt_max = rtt;
    st_rtt_sum += rtt;
    st_rtt_n++;
  } else if (f->type == XL_FAULT && f->len >= 9) {
    st_ra4_boot = xl_get32(f->payload);
  } else if (f->type == XL_CONFIG_ACK && f->len >= 2) {
    if (f->payload[1]) st_config_acks++; else st_config_nacks++;
  }
  (void)now;
  fwd_t m;
  size_t n = xl_encode(m.b, sizeof(m.b), f->type, f->session, f->seq, f->payload, f->len);
  if (n == 0) return;
  m.len = (uint16_t)n;
  if (xQueueSend(s_fwd, &m, 0) != pdTRUE) st_fwd_drop++;
}

// F11: frames of the previous ESP boot, copied out of RTC memory at boot (RTC slots are reused).
static uint8_t s_old[REPLAY_SLOTS][32];
static uint16_t s_old_len[REPLAY_SLOTS];
static uint32_t s_old_n;

static void replay_old_frames(void)
{
  for (uint32_t i = 0; i < s_old_n; i++) {
    if (s_old_len[i] > 0 && s_old_len[i] <= 32) {
      tx_raw(s_old[i], s_old_len[i]);
      st_bad_sent[7]++;
    }
  }
}

static void mbox_task(void*)
{
  st_wdt_rc = (int)esp_task_wdt_init(KX_WDT_S, true);
  esp_task_wdt_add(NULL);
  static xl_decoder_t dec;
  xl_decoder_init(&dec);
  uint32_t next_ping = 0, next_tsync = 0, next_bad = 0;
  bool replay_before = s_rtc.replay_pending != 0;
  bool replay_after = replay_before;
  uint8_t buf[256];
  if (replay_before) {  // F11: frames of the previous ESP boot, before any HELLO
    s_old_n = s_rtc.replay_n < REPLAY_SLOTS ? s_rtc.replay_n : REPLAY_SLOTS;
    memcpy(s_old, s_rtc.replay, sizeof(s_old));
    memcpy(s_old_len, s_rtc.replay_len, sizeof(s_old_len));
    replay_old_frames();
  }
  s_rtc.replay_n = 0;
  s_rtc.replay_pending = 0;
  for (;;) {
    if (active(F_HANG)) {
      for (;;) {  // F7b: stuck task, never feeds the watchdog -> TWDT panic -> reboot
      }
    }
    esp_task_wdt_reset();
    uint32_t now = millis();
    if (!active(F_NOREAD)) {
      int n;
      while ((n = Serial1.read(buf, sizeof(buf))) > 0) {
        st_uart_rx_bytes += n;
        for (int i = 0; i < n; i++) {
          if (xl_decode_byte(&dec, buf[i]) == XLD_FRAME && (dec.f.type & 0x80u)) on_ra4_frame(&dec.f);
        }
      }
      st_rx_crc = dec.err_crc;
      st_rx_len = dec.err_len;
      st_rx_ver = dec.err_version;
      st_rx_skipped = dec.bytes_skipped;
    }
    if (s_ra4_epoch != 0 && !s_established && (now - s_hs_sent_ms) > KX_HANDSHAKE_RETRY_MS) {
      start_handshake();  // no confirmation: fresh session id
    }
    if (s_established) {
      if (replay_after) {  // F11: the old frames again, now inside a live session
        replay_old_frames();
        replay_after = false;
      }
      if ((int32_t)(now - next_ping) >= 0) {
        uint8_t p[4];
        xl_put32(p, (uint32_t)micros());
        tx_frame(XL_PING, p, 4);
        next_ping = now + KX_PING_MS;
      }
      if ((int32_t)(now - next_tsync) >= 0) {
        uint8_t p[8];
        uint64_t us = esp_timer_get_time();
        xl_put32(p, (uint32_t)us);
        xl_put32(&p[4], (uint32_t)(us >> 32));
        tx_frame(XL_TIME_SYNC, p, 8);
        next_tsync = now + KX_TSYNC_MS;
      }
      if (s_cfg_pending) {
        uint8_t p[8] = {(uint8_t)s_cfg_param, 0, 0, 0};
        xl_put32(&p[4], s_cfg_value);
        tx_frame(XL_CONFIG_REQ, p, 8);
        s_cfg_pending = 0;
      }
      if (active(F_FLOOD)) {  // F5: valid in-session PINGs as fast as the UART takes them
        for (int i = 0; i < 64 && Serial1.availableForWrite() > 32; i++) {
          uint8_t p[4];
          xl_put32(p, 0);
          tx_frame(XL_PING, p, 4);
        }
      }
    }
    if (active(F_MALFORMED) && (int32_t)(now - next_bad) >= 0) {  // F4
      uint32_t per_tick = f_malformed_rate / 500 + 1;
      for (uint32_t i = 0; i < per_tick; i++) send_malformed();
      next_bad = now + 2;
    }
    vTaskDelay(1);
  }
}

// ---------------- net task ----------------
static WiFiUDP s_ctrl, s_sink;
static IPAddress s_peer;
static uint16_t s_peer_port;
static volatile uint32_t s_ap_restarts;

static void busy_task(void* arg)
{
  uint32_t until = (uint32_t)(uintptr_t)arg;
  uint32_t t0 = millis();
  while ((int32_t)(millis() - until) < 0) {
    if (millis() - t0 >= 20) {  // saturation, not a hang: 1 tick every 20 ms keeps IDLE0 alive
      vTaskDelay(1);
      t0 = millis();
    }
  }
  vTaskDelete(NULL);
}

static void ap_up(void)
{
  WiFi.mode(WIFI_AP);
  WiFi.softAP(KX_AP_SSID, KX_AP_PASS, 6, 0, 4);
}

static int stats_text(char* o, size_t cap)
{
  uint32_t n = st_rtt_n;
  return snprintf(o, cap,
      "stats boot=%u reset_reason=%d up_ms=%u ra4_epoch=%u session=%u est=%u handshakes=%u "
      "rx_frames=%u rx_crc=%u rx_len=%u rx_ver=%u rx_skipped=%u uart_rx=%u hb=%u pong=%u "
      "rtt_us_min=%u rtt_us_avg=%u rtt_us_max=%u tx_frames=%u tx_bytes=%u valid_sent=%u "
      "bad=%u,%u,%u,%u,%u,%u,%u,%u fwd_drop=%u sink_pkts=%u sink_bytes=%llu ctrl_pkts=%u "
      "cfg_ack=%u cfg_nack=%u ra4_ctl_jobs=%u ra4_ctl_miss=%u ra4_degraded=%u heap=%u heap_min=%u "
      "ap_restarts=%u stations=%u wdt_rc=%d resyncs=%u",
      s_rtc.boot_count, (int)esp_reset_reason(), (unsigned)millis(), s_ra4_epoch, s_session, s_established,
      st_handshakes, st_rx_frames, st_rx_crc, st_rx_len, st_rx_ver, st_rx_skipped, st_uart_rx_bytes, st_hb,
      st_pong, n ? st_rtt_min : 0, n ? (uint32_t)(st_rtt_sum / n) : 0, st_rtt_max, st_tx_frames, st_tx_bytes,
      st_valid_sent, st_bad_sent[0], st_bad_sent[1], st_bad_sent[2], st_bad_sent[3], st_bad_sent[4],
      st_bad_sent[5], st_bad_sent[6], st_bad_sent[7], st_fwd_drop, st_sink_pkts,
      (unsigned long long)st_sink_bytes, st_ctrl_pkts, st_config_acks, st_config_nacks, st_ra4_ctl_jobs,
      st_ra4_ctl_miss, st_ra4_degraded, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
      s_ap_restarts, (unsigned)WiFi.softAPgetStationNum(), st_wdt_rc, st_resyncs);
}

static void reply(const char* s)
{
  s_ctrl.beginPacket(s_ctrl.remoteIP(), s_ctrl.remotePort());
  s_ctrl.write((const uint8_t*)s, strlen(s));
  s_ctrl.endPacket();
}

static void handle_ctrl(char* cmd)
{
  st_ctrl_pkts++;
  char out[900];
  unsigned a = 0, b = 0;
  char name[16] = {0};
  if (!strncmp(cmd, "sub", 3)) {
    s_peer = s_ctrl.remoteIP();
    s_peer_port = s_ctrl.remotePort();
    reply("ok sub");
  } else if (!strncmp(cmd, "stats", 5)) {
    stats_text(out, sizeof(out));
    reply(out);
  } else if (sscanf(cmd, "config %u %u", &a, &b) == 2) {
    s_cfg_param = a;
    s_cfg_value = b;
    s_cfg_pending = 1;
    reply("ok config");
  } else if (sscanf(cmd, "fault %15s %u %u", name, &a, &b) >= 1) {
    uint32_t now = millis();
    if (!strcmp(name, "cpu")) {  // F2: a busy task on each core, priority b (default 10), a ms
      UBaseType_t pr = b ? b : 10;
      xTaskCreatePinnedToCore(busy_task, "busy0", 2048, (void*)(uintptr_t)(now + a), pr, NULL, 0);
      xTaskCreatePinnedToCore(busy_task, "busy1", 2048, (void*)(uintptr_t)(now + a), pr, NULL, 1);
    } else if (!strcmp(name, "mem")) {  // F3: hold heap down to b bytes free for a ms
      static void* blocks[512];
      int nb = 0;
      uint32_t floor_b = b ? b : 8192;
      while (nb < 512 && ESP.getFreeHeap() > floor_b + 2048) {
        void* p = malloc(2048);
        if (!p) break;
        memset(p, 0x5A, 2048);
        blocks[nb++] = p;
      }
      snprintf(out, sizeof(out), "ok mem held=%d heap=%u", nb, (unsigned)ESP.getFreeHeap());
      reply(out);
      vTaskDelay(pdMS_TO_TICKS(a));
      for (int i = 0; i < nb; i++) free(blocks[i]);
      return;
    } else if (!strcmp(name, "malformed")) {  // F4
      f_malformed_rate = b ? b : 200;
      f_until_ms[F_MALFORMED] = now + a;
    } else if (!strcmp(name, "flood")) {  // F5
      f_until_ms[F_FLOOD] = now + a;
    } else if (!strcmp(name, "rebootloop")) {  // F6: a reboots, b ms after each boot
      s_rtc.reboot_loop_left = a > 0 ? a - 1 : 0;  // this restart is the first of a
      s_rtc.reboot_loop_delay_ms = b ? b : 2000;
      reply("ok rebootloop");
      delay(50);
      esp_restart();
    } else if (!strcmp(name, "crash")) {  // F7: NULL write -> panic -> reboot
      reply("ok crash");
      delay(50);
      *(volatile uint32_t*)0 = 1;
    } else if (!strcmp(name, "hang")) {  // F7b: mailbox task stops feeding the task watchdog
      f_until_ms[F_HANG] = now + 60000;
    } else if (!strcmp(name, "apstorm")) {  // F8: a AP restarts, b ms apart
      reply("ok apstorm");
      for (unsigned i = 0; i < a; i++) {
        WiFi.softAPdisconnect(true);
        vTaskDelay(pdMS_TO_TICKS(b ? b : 500));
        ap_up();
        s_ap_restarts++;
        vTaskDelay(pdMS_TO_TICKS(b ? b : 500));
      }
      s_ctrl.begin(KX_CTRL_PORT);
      s_sink.begin(KX_SINK_PORT);
      return;
    } else if (!strcmp(name, "noread")) {  // F9
      f_until_ms[F_NOREAD] = now + a;
    } else if (!strcmp(name, "notx")) {  // F10
      f_until_ms[F_NOTX] = now + a;
    } else if (!strcmp(name, "replay")) {  // F11: reboot, then replay the previous session's frames
      s_rtc.replay_pending = 1;
      reply("ok replay");
      delay(50);
      esp_restart();
    } else if (!strcmp(name, "txlow")) {  // F12: hold the TX line low (break) for a ms
      f_until_ms[F_NOREAD] = f_until_ms[F_NOTX] = now + a + 200;  // mbox task keeps off UART1
      vTaskDelay(pdMS_TO_TICKS(20));
      Serial1.end();
      pinMode(KX_UART_TX, OUTPUT);
      digitalWrite(KX_UART_TX, LOW);
      reply("ok txlow");
      vTaskDelay(pdMS_TO_TICKS(a));
      Serial1.begin(KX_BAUD, SERIAL_8N1, KX_UART_RX, KX_UART_TX);
      return;
    } else if (!strcmp(name, "reboot")) {
      reply("ok reboot");
      delay(50);
      esp_restart();
    } else {
      reply("err fault");
      return;
    }
    reply("ok fault");
  } else {
    reply("err cmd");
  }
}

static void net_task(void*)
{
  ap_up();
  s_ctrl.begin(KX_CTRL_PORT);
  s_sink.begin(KX_SINK_PORT);
  static uint8_t sink_buf[1500];
  char cmd[128];
  for (;;) {
    int n = s_ctrl.parsePacket();
    if (n > 0) {
      int r = s_ctrl.read((uint8_t*)cmd, sizeof(cmd) - 1);
      cmd[r > 0 ? r : 0] = 0;
      handle_ctrl(cmd);
    }
    while ((n = s_sink.parsePacket()) > 0) {
      s_sink.read(sink_buf, sizeof(sink_buf));
      st_sink_pkts++;
      st_sink_bytes += (uint32_t)n;
    }
    fwd_t m;
    while (xQueueReceive(s_fwd, &m, 0) == pdTRUE) {
      if (s_peer_port) {
        s_ctrl.beginPacket(s_peer, s_peer_port);
        s_ctrl.write(m.b, m.len);
        s_ctrl.endPacket();
      }
    }
    vTaskDelay(1);
  }
}

static void reboot_loop_task(void*)
{
  vTaskDelay(pdMS_TO_TICKS(s_rtc.reboot_loop_delay_ms));
  s_rtc.reboot_loop_left--;
  esp_restart();
}

void kx_start(volatile uint32_t* usb_baud)
{
  s_usb_baud = usb_baud;
  if (s_rtc.magic != RTC_MAGIC) {
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = RTC_MAGIC;
  }
  s_rtc.boot_count++;
  s_fwd = xQueueCreate(32, sizeof(fwd_t));
  Serial1.setRxBufferSize(4096);
  Serial1.setTxBufferSize(4096);
  Serial1.begin(KX_BAUD, SERIAL_8N1, KX_UART_RX, KX_UART_TX);
  xTaskCreatePinnedToCore(mbox_task, "kx_mbox", 6144, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(net_task, "kx_net", 8192, NULL, 2, NULL, 0);
  if (s_rtc.reboot_loop_left > 0) {
    xTaskCreatePinnedToCore(reboot_loop_task, "kx_rbl", 2048, NULL, 1, NULL, 0);
  }
}

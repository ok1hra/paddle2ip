// keyer.cpp — iambic keyer + sidetone, task pinned to core 1 with a 1 ms tick.
//
// Iambic logic is the OI3 keyer (K3NG port, modes A/B, CMOS Super Keyer
// timing) rewritten as a non-blocking state machine.
//
// Edge times are the keyer's IDEAL times: an element that follows another one
// starts exactly at the end of the previous inter-element gap, and its key-up
// is exactly 1 or 3 dits later, whatever tick actually handled it. Only the
// first element after idle starts at the tick that saw the paddle. These ideal
// times go into the KEY packets; the sidetone follows them within one tick.
//
// Test message: the UI can start "TEST", played with the same ideal timing;
// any paddle or the straight key aborts it (the current element is finished).
//
// Element trace (display "MONITOR"): every key-up appends '.' or '-' by the
// mark length (< 2 dits = dot), every key-down after a gap of >= 2 dits a ' '
// (letter) or after >= 5 dits " / " (word). Same edges as in the KEY packets,
// so it works for the paddle, the test message and the straight key.
//
// Sidetone: LEDC runs permanently at the chosen frequency; keying only switches
// the duty between 0 and the volume duty (no M5.Speaker, it buffers audio).
// The tone's fundamental is proportional to sin(pi * duty), maximum at 50 %, so
// vol 1-100 maps linearly in dB to the amplitude and the duty is its inverse.
#include "shared.h"
#include "rkey_proto.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static const uint8_t LEDC_CH_BUZZ = 0;
static const uint8_t LEDC_CH_HP   = 1;          // same LEDC timer as CH_BUZZ
static const uint8_t LEDC_BITS    = 14;          // 80 MHz / 2^14 = 4.9 kHz > TONE_MAX
static const float   VOL_RANGE_DB = 40.0f;       // vol 100 = 0 dB, each step of 5 = 2 dB

// ---------------- edge ring (keyer -> net) ----------------
#define EDGE_RING 32                             // power of two, >= RKEY_MAX_EV
static portMUX_TYPE edgeMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t ring[EDGE_RING];
static uint16_t nextIdx   = 0;                   // index of the next edge (session-wide)
static uint32_t edgeCount = 0;                   // total edges since boot
static uint32_t lastEdgeUs = 0;
static volatile bool keyDown = false;
static TaskHandle_t netTask = nullptr;

static uint32_t vol_duty(uint8_t vol) {
  if (!vol) return 0;
  float a = powf(10.0f, (vol - 100) * VOL_RANGE_DB / (100 * 20.0f));   // amplitude 0..1
  return (uint32_t)(asinf(a) / (float)M_PI * ((1u << LEDC_BITS) - 1) + 0.5f);
}

static void sidetone(bool on) {
  uint32_t duty = on ? vol_duty(g_set.vol) : 0;
  ledcWrite(LEDC_CH_BUZZ, duty);
  if (g_set.hpEnable) ledcWrite(LEDC_CH_HP, duty);
}

// ---------------- element trace (keyer -> display) ----------------
#define TRACE_LEN 48
static char     trace[TRACE_LEN + 1];            // guarded by edgeMux
static uint8_t  traceN = 0;
static uint32_t traceDownUs = 0;

static inline uint32_t dit_us();

static void trace_put(char c) {                  // edgeMux held
  if (traceN == TRACE_LEN) { memmove(trace, trace + 1, TRACE_LEN - 1); traceN--; }
  trace[traceN++] = c;
}

static void trace_edge(uint32_t t, bool down) {  // edgeMux held
  uint32_t dit = dit_us();
  if (down) {
    uint32_t gap = t - lastEdgeUs;               // lastEdgeUs = previous key-up
    if (traceN && edgeCount) {
      if (gap >= 5 * dit)      { trace_put(' '); trace_put('/'); trace_put(' '); }
      else if (gap >= 2 * dit) trace_put(' ');
    }
    traceDownUs = t;
  } else {
    trace_put((t - traceDownUs) < 2 * dit ? '.' : '-');
  }
}

uint8_t keyer_trace(char* out, uint8_t max) {
  portENTER_CRITICAL(&edgeMux);
  uint8_t n = traceN < max ? traceN : max;
  memcpy(out, trace + traceN - n, n);
  portEXIT_CRITICAL(&edgeMux);
  out[n] = 0;
  return n;
}

void keyer_trace_clear() {
  portENTER_CRITICAL(&edgeMux);
  traceN = 0;
  portEXIT_CRITICAL(&edgeMux);
}

static void emit(uint32_t t, bool down) {
  sidetone(down);
  uint32_t ev = (t & ~1u) | (down ? 1u : 0u);
  portENTER_CRITICAL(&edgeMux);
  trace_edge(t, down);
  ring[nextIdx & (EDGE_RING - 1)] = ev;
  nextIdx++;
  edgeCount++;
  lastEdgeUs = t;
  keyDown = down;
  portEXIT_CRITICAL(&edgeMux);
  if (netTask) xTaskNotifyGive(netTask);          // send a KEY packet now
}

uint8_t keyer_collect(uint32_t nowUs, uint32_t* ev, uint16_t* idx0) {
  portENTER_CRITICAL(&edgeMux);
  uint32_t avail = edgeCount < EDGE_RING ? edgeCount : EDGE_RING;
  uint16_t i = nextIdx;
  uint8_t  k = 0;
  while (k < RKEY_MAX_EV && k < avail) {
    uint32_t e = ring[(uint16_t)(i - 1) & (EDGE_RING - 1)];
    bool keepDown = (k == 0) && RKEY_EV_DOWN(e);  // never report "idle" while the key is down
    if (!keepDown && (int32_t)(nowUs - RKEY_EV_TIME(e)) >= 500000) break;
    i--; k++;
  }
  for (uint8_t j = 0; j < k; j++) ev[j] = ring[(uint16_t)(i + j) & (EDGE_RING - 1)];
  *idx0 = i;
  portEXIT_CRITICAL(&edgeMux);
  return k;
}

uint32_t keyer_idle_us(uint32_t nowUs) {
  portENTER_CRITICAL(&edgeMux);
  uint32_t r = edgeCount ? (nowUs - lastEdgeUs) : 0xFFFFFFFFu;
  bool down = keyDown;
  portEXIT_CRITICAL(&edgeMux);
  return down ? 0 : r;
}

bool keyer_key_down() { return keyDown; }

void keyer_set_tone(uint16_t hz) {
  ledcChangeFrequency(LEDC_CH_BUZZ, hz, LEDC_BITS);   // CH_HP shares the timer
}

// ---------------- iambic state machine ----------------
enum KState : uint8_t { K_IDLE, K_MARK, K_GAP, K_STRAIGHT };
static KState  kState = K_IDLE;
static uint32_t kStart = 0, kEnd = 0;            // current phase, ideal µs
static uint8_t ditBuf = 0, dahBuf = 0;
static uint8_t beingSent = 0;                    // 0 none, 1 dit, 2 dah (kept through the gap)
static uint8_t curType = 0;
static uint8_t lastSent = 1;                     // for squeeze alternation
static uint8_t iambicAFlag = 0;
static bool    pDit = false, pDah = false;       // paddles sampled this tick
const uint8_t  CMOS_SUPER_KEYER_B_TIMING_PERCENT = 33;

// ---------------- test message ----------------
static const char MSG_TEST[] = "- . ... -";      // TEST: '.' dit, '-' dah, ' ' letter gap
static volatile bool msgReq = false;              // set by the UI task
static const char* msg = nullptr;                 // next symbol, nullptr = no message

void keyer_send_test()  { msgReq = true; }
bool keyer_msg_active() { return msgReq || msg != nullptr; }

static inline uint32_t dit_us() {
  uint8_t w = g_set.wpm;
  if (w < WPM_MIN) w = WPM_MIN;
  if (w > WPM_MAX) w = WPM_MAX;
  return 1200000UL / w;
}

// During an element read only the OPPOSITE paddle (squeeze buffering).
static void check_paddles() {
  if (beingSent == 1)      { if (pDah) dahBuf = 1; }
  else if (beingSent == 2) { if (pDit) ditBuf = 1; }
  else { if (pDit) ditBuf = 1; if (pDah) dahBuf = 1; }
  if (g_set.mode == 'A' && beingSent && pDit && pDah) iambicAFlag = 1;
}

// K3NG CMOS Super Keyer timing: in the first 33 % of a phase an early squeeze
// is cleared, after that the opposite paddle latches normally.
static void check_paddles_cmos(uint32_t elapsed, uint32_t total) {
  if (beingSent == 0 || total == 0) { check_paddles(); return; }
  if (g_set.mode == 'A' && pDit && pDah) iambicAFlag = 1;
  if ((uint64_t)elapsed * 100u >= (uint64_t)total * CMOS_SUPER_KEYER_B_TIMING_PERCENT) {
    if (beingSent == 1)      { if (pDah) dahBuf = 1; }
    else if (beingSent == 2) { if (pDit) ditBuf = 1; }
  } else if (pDit && pDah) {
    ditBuf = 0; dahBuf = 0;
  }
}

// Start the next element at ideal time t if a paddle asks for one.
static bool start_element(uint32_t t) {
  check_paddles();
  if (!ditBuf && !dahBuf) return false;
  uint8_t ty;
  if (ditBuf && dahBuf) ty = (lastSent == 1) ? 2 : 1;
  else                  ty = ditBuf ? 1 : 2;
  if (ty == 1) ditBuf = 0; else dahBuf = 0;
  curType = beingSent = ty;
  kStart = t;
  kEnd   = t + (ty == 1 ? 1u : 3u) * dit_us();
  kState = K_MARK;
  emit(t, true);
  return true;
}

// Next message element at ideal time t; false = message done.
static bool msg_element(uint32_t t) {
  if (!msg || !*msg) { msg = nullptr; return false; }
  uint8_t ty = (*msg++ == '-') ? 2 : 1;
  curType = beingSent = ty;
  kStart = t;
  kEnd   = t + (ty == 1 ? 1u : 3u) * dit_us();
  kState = K_MARK;
  emit(t, true);
  return true;
}

static void keyer_tick(uint32_t now) {
  bool a = gpio_get_level((gpio_num_t)PIN_DIT) == 0;
  bool b = gpio_get_level((gpio_num_t)PIN_DAH) == 0;
  pDit = g_set.swap ? b : a;
  pDah = g_set.swap ? a : b;
  bool sk = g_set.skEnable && gpio_get_level((gpio_num_t)PIN_SK) == 0;
  if (pDit || pDah || sk) { msg = nullptr; msgReq = false; }   // the operator takes over

  for (uint8_t guard = 0; guard < 6; guard++) {   // a late tick may cross several phase ends
    switch (kState) {
      case K_IDLE:
        if (sk) { kState = K_STRAIGHT; emit(now, true); return; }
        if (start_element(now)) return;
        if (msgReq) { msgReq = false; msg = MSG_TEST; msg_element(now); }
        return;

      case K_STRAIGHT:                            // measured times; the paddle waits
        if (!sk) { emit(now, false); kState = K_IDLE; }
        return;

      case K_MARK:
        if ((int32_t)(now - kEnd) < 0) { check_paddles_cmos(now - kStart, kEnd - kStart); return; }
        emit(kEnd, false);
        kStart = kEnd;
        kEnd   = kStart + dit_us();               // inter-element gap
        if (msg && *msg == ' ') { msg++; kEnd += 2 * dit_us(); }   // letter gap
        kState = K_GAP;
        break;

      case K_GAP:
        if ((int32_t)(now - kEnd) < 0) { check_paddles_cmos(now - kStart, kEnd - kStart); return; }
        beingSent = 0;
        lastSent  = curType;
        // Iambic A: both paddles squeezed during the element and both released
        // by its end -> drop the buffered element.
        if (g_set.mode == 'A' && iambicAFlag && !pDit && !pDah) {
          iambicAFlag = 0; ditBuf = 0; dahBuf = 0;
        }
        if (!(msg ? msg_element(kEnd) : start_element(kEnd))) { kState = K_IDLE; return; }
        break;
    }
  }
}

static void keyer_task(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last, 1);                    // CONFIG_FREERTOS_HZ = 1000
    keyer_tick((uint32_t)esp_timer_get_time());
  }
}

void keyer_begin(TaskHandle_t notifyOnEdge) {
  netTask = notifyOnEdge;
  const gpio_num_t in[] = { (gpio_num_t)PIN_DIT, (gpio_num_t)PIN_DAH, (gpio_num_t)PIN_SK };
  for (gpio_num_t p : in) {
    gpio_reset_pin(p);
    gpio_set_direction(p, GPIO_MODE_INPUT);
    gpio_set_pull_mode(p, GPIO_PULLUP_ONLY);
  }
  ledcSetup(LEDC_CH_BUZZ, g_set.toneHz, LEDC_BITS);
  ledcAttachPin(PIN_BUZZ, LEDC_CH_BUZZ);
  ledcWrite(LEDC_CH_BUZZ, 0);
  if (g_set.hpEnable) {
    ledcSetup(LEDC_CH_HP, g_set.toneHz, LEDC_BITS);
    ledcAttachPin(PIN_HP, LEDC_CH_HP);
    ledcWrite(LEDC_CH_HP, 0);
  }
  xTaskCreatePinnedToCore(keyer_task, "keyer", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
}

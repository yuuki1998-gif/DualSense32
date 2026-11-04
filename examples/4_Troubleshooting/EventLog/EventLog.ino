#include <ds_bt_bluedroid.h>

/*
 * EventLog — event log for everything the library decodes and drives.
 *
 *   [  2.14s] CROSS     down
 *   [  2.31s] CROSS     up
 *   [  3.02s] LSTICK    (  42, -118)
 *   [  3.44s] L2         142
 *   [  4.10s] TOUCH     down  id19 (1133,  809)
 *   [  4.85s] TOUCH     up
 *   [  5.20s] BATTERY   10
 *   [  6.60s] MOTION    gyro(  945,  -31, -232)  |a|=8186
 *
 * Analog values are only logged once they have moved a meaningful amount, so a
 * resting stick or a drifting gyro never fills the screen.
 *
 *   OPTIONS        snapshot of everything right now
 *   PS (hold 1 s)  feedback check: lightbar, rumble, all four trigger effects
 *
 * Running quietly underneath is a set of consistency checks. Numbers that move
 * do not prove the decode is right — a wrong offset also produces movement.
 * These compare fields that must agree with each other, and print a line only
 * when they disagree, so silence is the pass condition.
 */

/* Resting lightbar colour, restored after the feedback check. */
static const uint8_t IDLE_R = 0, IDLE_G = 90, IDLE_B = 140;

#if !DS_HAS_BT_BLUEDROID
void setup() { Serial.begin(115200); }
void loop() {
  Serial.println("Needs Bluetooth Classic: use the original ESP32.");
  delay(4000);
}
#else

/* How far an analog input must move before it is worth a line. */
static const int STICK_STEP = 12;
static const int TRIGGER_STEP = 16;
static const int TOUCH_STEP = 40;
/* Raw units here: no calibration is read over Bluetooth yet, so the
 * gyro is about 16 LSB per deg/s and this is roughly 25 deg/s. */
static const long GYRO_WAKE = 400;

static ds_transport_t* tr;
static ds_state_t st;
static uint8_t seq;
static uint8_t trigL[11], trigR[11];

static void stamp() { Serial.printf("[%7.2fs] ", millis() / 1000.0f); }

/* ---------------------------------------------------------------- output -- */

static void send(uint8_t r, uint8_t g, uint8_t b, uint8_t rl, uint8_t rr, uint8_t leds) {
  ds_output_t out;
  ds_output_init(&out);
  out.set_lightbar = true;    out.red = r; out.green = g; out.blue = b;
  out.set_rumble = true;      out.rumble_left = rl; out.rumble_right = rr;
  out.set_player_leds = true; out.player_leds = leds;
  out.set_trigger_left = true;  memcpy(out.trigger_left, trigL, 11);
  out.set_trigger_right = true; memcpy(out.trigger_right, trigR, 11);
  out.sequence = seq++;   /* inside the CRC, so it is set before building */

  uint8_t buf[DS_OUT_MAX_SIZE];
  uint16_t n = ds_build_output(&out, DS_LINK_BT, buf, sizeof(buf));
  if (n) tr->send(tr, buf, n);
}

static void idleLed() { send(IDLE_R, IDLE_G, IDLE_B, 0, 0, 0); }

/* --------------------------------------------------------------- buttons -- */

static const char* BTN[] = {
  "UP", "RIGHT", "DOWN", "LEFT", "SQUARE", "CROSS", "CIRCLE", "TRIANGLE",
  "L1", "R1", "L2", "R2", "CREATE", "OPTIONS", "L3", "R3", "PS", "TOUCHPAD", "MUTE",
};
static const int BTN_COUNT = 19;
static bool btnPrev[BTN_COUNT];

static bool btnState(int i) {
  const ds_buttons_t* b = &st.button;
  switch (i) {
    case 0: return b->up;       case 1: return b->right;   case 2: return b->down;
    case 3: return b->left;     case 4: return b->square;  case 5: return b->cross;
    case 6: return b->circle;   case 7: return b->triangle;case 8: return b->l1;
    case 9: return b->r1;       case 10: return b->l2;     case 11: return b->r2;
    case 12: return b->create;  case 13: return b->options;case 14: return b->l3;
    case 15: return b->r3;      case 16: return b->ps;     case 17: return b->touchpad;
    case 18: return b->mute;    default: return false;
  }
}

static void logButtons() {
  for (int i = 0; i < BTN_COUNT; i++) {
    bool now = btnState(i);
    if (now == btnPrev[i]) continue;
    btnPrev[i] = now;
    stamp();
    Serial.printf("%-9s %s\n", BTN[i], now ? "down" : "up");
  }
}

/* ---------------------------------------------------------------- analog -- */

/* Sticks are reported raw 0-255; show them centred so 0 means untouched. */
static int centred(uint8_t v) { return (int)v - 128; }

static void logSticks() {
  static int lx = 0, ly = 0, rx = 0, ry = 0;

  int nlx = centred(st.analog.lx), nly = centred(st.analog.ly);
  if (abs(nlx - lx) >= STICK_STEP || abs(nly - ly) >= STICK_STEP) {
    lx = nlx; ly = nly;
    stamp(); Serial.printf("%-9s (%4d,%5d)\n", "LSTICK", lx, ly);
  }

  int nrx = centred(st.analog.rx), nry = centred(st.analog.ry);
  if (abs(nrx - rx) >= STICK_STEP || abs(nry - ry) >= STICK_STEP) {
    rx = nrx; ry = nry;
    stamp(); Serial.printf("%-9s (%4d,%5d)\n", "RSTICK", rx, ry);
  }
}

static void logTriggers() {
  static int l2 = 0, r2 = 0;

  int n = st.analog.l2;
  if (abs(n - l2) >= TRIGGER_STEP || (n == 0) != (l2 == 0)) {
    l2 = n;
    stamp(); Serial.printf("%-9s %4d\n", "L2", l2);
  }
  n = st.analog.r2;
  if (abs(n - r2) >= TRIGGER_STEP || (n == 0) != (r2 == 0)) {
    r2 = n;
    stamp(); Serial.printf("%-9s %4d\n", "R2", r2);
  }
}

static void logTouch() {
  static bool was = false, was2 = false;
  static int lastX = 0, lastY = 0;

  bool now = st.touch[0].active;
  int x = st.touch[0].x, y = st.touch[0].y;
  if (now && !was) {
    stamp(); Serial.printf("%-9s down  id%-3u (%4d,%5d)\n", "TOUCH", st.touch[0].id, x, y);
    lastX = x; lastY = y;
  } else if (now && (abs(x - lastX) >= TOUCH_STEP || abs(y - lastY) >= TOUCH_STEP)) {
    stamp(); Serial.printf("%-9s move       (%4d,%5d)\n", "TOUCH", x, y);
    lastX = x; lastY = y;
  } else if (!now && was) {
    stamp(); Serial.printf("%-9s up\n", "TOUCH");
  }
  was = now;

  bool now2 = st.touch[1].active;
  if (now2 != was2) {
    stamp();
    Serial.printf("%-9s %s  id%-3u (%4u,%5u)\n", "TOUCH2", now2 ? "down" : "up  ",
                  st.touch[1].id, st.touch[1].x, st.touch[1].y);
    was2 = now2;
  }
}

static long accelMag() {
  long ax = st.motion.accel[0], ay = st.motion.accel[1], az = st.motion.accel[2];
  return (long)sqrt((double)(ax * ax + ay * ay + az * az));
}

static void logMotion() {
  static unsigned long lastAt = 0;
  if (!st.has_motion) return;

  long gx = st.motion.gyro[0], gy = st.motion.gyro[1], gz = st.motion.gyro[2];
  if (labs(gx) < GYRO_WAKE && labs(gy) < GYRO_WAKE && labs(gz) < GYRO_WAKE) return;
  if (millis() - lastAt < 1000) return;
  lastAt = millis();

  stamp();
  Serial.printf("%-9s gyro(%7ld,%7ld,%7ld)  |a|=%ld\n", "MOTION", gx, gy, gz, accelMag());
}

static void logStatus() {
  static uint8_t lastReport = 0, lastBat = 255, lastChg = 255;

  if (st.report_id != lastReport) {
    lastReport = st.report_id;
    stamp();
    Serial.printf("%-9s 0x%02X%s\n", "REPORT", lastReport,
                  lastReport == 0x31 ? "  (full: battery/motion/touch)"
                                     : "  (compatibility: sticks+buttons only)");
  }
  if (st.status.valid && (st.status.battery != lastBat || st.status.charging != lastChg)) {
    lastBat = st.status.battery;
    lastChg = st.status.charging;
    stamp();
    Serial.printf("%-9s %u%s\n", "BATTERY", lastBat,
                  lastChg == 1 ? "  charging" : (lastChg == 2 ? "  full" : ""));
  }
}

/* ----------------------------------------------------------- consistency -- */

/* Fields that must agree with each other. A wrong report offset still produces
 * numbers that move, but it cannot keep these in step. Prints only on a
 * disagreement, and only once per kind, so silence means the decode is sound. */
static void consistency() {
  static bool told[6];

  struct { int idx; bool bad; const char* msg; } c[] = {
    { 0, (st.analog.l2 > 30) != st.button.l2 && st.analog.l2 != 0,
         "L2 analog and digital disagree -> trigger offset" },
    { 1, (st.analog.r2 > 30) != st.button.r2 && st.analog.r2 != 0,
         "R2 analog and digital disagree -> trigger offset" },
    { 2, st.button.cross && st.button.circle && st.button.square && st.button.triangle,
         "all four face buttons at once -> button byte offset" },
    { 3, (st.button.up + st.button.down + st.button.left + st.button.right) > 2,
         "three d-pad directions at once -> hat decode" },
    { 4, st.has_motion && (accelMag() < 6500 || accelMag() > 10000),
         "accel magnitude far from 1 g -> motion offset or scale" },
    { 5, st.report_id == 0x31 && !st.status.valid,
         "0x31 report without valid status -> status offset" },
  };

  for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
    if (!c[i].bad || told[c[i].idx]) continue;
    told[c[i].idx] = true;
    stamp();
    Serial.printf("!! INCONSISTENT: %s\n", c[i].msg);
  }
}

/* -------------------------------------------------------------- snapshot -- */

static void snapshot() {
  Serial.println("--- snapshot ------------------------------------");
  Serial.printf("  report 0x%02X   battery %s%u%s\n", st.report_id,
                st.status.valid ? "" : "n/a:", st.status.battery,
                st.status.charging == 1 ? " charging" : "");
  Serial.printf("  LS (%4d,%5d)   RS (%4d,%5d)\n",
                centred(st.analog.lx), centred(st.analog.ly),
                centred(st.analog.rx), centred(st.analog.ry));
  Serial.printf("  L2 %3u   R2 %3u\n", st.analog.l2, st.analog.r2);

  Serial.print("  held:");
  bool any = false;
  for (int i = 0; i < BTN_COUNT; i++)
    if (btnState(i)) { Serial.printf(" %s", BTN[i]); any = true; }
  Serial.println(any ? "" : " (none)");

  Serial.printf("  gyro (%7ld,%7ld,%7ld)   accel (%5d,%5d,%5d)  |a|=%ld\n",
                (long)st.motion.gyro[0], (long)st.motion.gyro[1],
                (long)st.motion.gyro[2],
                st.motion.accel[0], st.motion.accel[1], st.motion.accel[2], accelMag());
  Serial.printf("  touch1 %s (%4u,%4u) id%-3u   touch2 %s (%4u,%4u)\n",
                st.touch[0].active ? "down" : "up  ", st.touch[0].x, st.touch[0].y,
                st.touch[0].id,
                st.touch[1].active ? "down" : "up  ", st.touch[1].x, st.touch[1].y);
  Serial.println("-------------------------------------------------");
}

/* -------------------------------------------------------- feedback check -- */

static void feedbackCheck() {
  Serial.println("=== FEEDBACK CHECK ==============================");

  Serial.println("  lightbar RED");   send(255, 0, 0, 0, 0, 0x01); delay(700);
  Serial.println("  lightbar GREEN"); send(0, 255, 0, 0, 0, 0x02); delay(700);
  Serial.println("  lightbar BLUE");  send(0, 0, 255, 0, 0, 0x04); delay(700);
  Serial.println("  rumble weak");    send(0, 0, 0, 90, 0, 0x08);  delay(600);
  Serial.println("  rumble strong");  send(0, 0, 0, 0, 200, 0x10); delay(600);
  send(0, 0, 0, 0, 0, 0);

  Serial.println("  trigger FEEDBACK   squeeze L2/R2 - steady resistance");
  ds5_trigger_feedback(trigL, 3, 6); ds5_trigger_feedback(trigR, 3, 6);
  send(0, 60, 60, 0, 0, 0); delay(2500);

  Serial.println("  trigger WEAPON     squeeze - should LOCK then break");
  ds5_trigger_weapon(trigL, 3, 6, 8); ds5_trigger_weapon(trigR, 3, 6, 8);
  send(60, 60, 0, 0, 0, 0); delay(2500);

  Serial.println("  trigger VIBRATION  squeeze - should buzz");
  ds5_trigger_vibration(trigL, 2, 6, 30); ds5_trigger_vibration(trigR, 2, 6, 30);
  send(60, 0, 60, 0, 0, 0); delay(2500);

  ds5_trigger_off(trigL); ds5_trigger_off(trigR);
  idleLed();
  Serial.println("=== done - lightbar back to idle colour ==========");
}

/* ------------------------------------------------------------------------- */

static void onReport(const uint8_t* rep, uint16_t len, void* user) {
  (void)user;
  ds_state_t tmp;
  if (ds_parse_input(rep, len, DS_LINK_BT, &tmp)) st = tmp;
}

static void onState(ds_conn_state_t s, void* user) {
  (void)user;
  static const char* N[] = {"disconnected", "connecting", "CONNECTED", "error"};
  stamp();
  Serial.printf("LINK      %s\n", N[s]);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("EventLog - event log");
  Serial.println("  OPTIONS       snapshot of everything");
  Serial.println("  PS (hold 1s)  feedback check (lightbar / rumble / triggers)");

  ds5_trigger_off(trigL);
  ds5_trigger_off(trigR);

  tr = ds_bt_bluedroid();
  tr->on_report = onReport;
  tr->on_state = onState;
  /* No address: the link inquires and takes whatever answers as a gamepad,
   * so put the pad in pairing mode (Create, then PS). To name one instead,
   * use ds_bt_begin(tr, "AA:BB:CC:DD:EE:FF") -- tr->begin() takes six bytes
   * in HCI wire order, which is the reverse of how an address is written and
   * an easy thing to get backwards. */
  if (!tr->begin(tr, NULL)) Serial.println("begin() failed");
  Serial.println("Ready. Press the PS button (or Create+PS to pair).");
}

void loop() {
  if (tr->state(tr) != DS_CONNECTED) { delay(50); return; }

  static bool greeted = false;
  if (!greeted) { greeted = true; idleLed(); }

  logButtons();
  logSticks();
  logTriggers();
  logTouch();
  logMotion();
  logStatus();
  consistency();

  static bool lastOpt = false;
  bool opt = st.button.options;
  if (opt && !lastOpt) snapshot();
  lastOpt = opt;

  static unsigned long psDown = 0;
  static bool psDone = false;
  if (st.button.ps) {
    if (!psDown) psDown = millis();
    if (!psDone && millis() - psDown > 1000) { psDone = true; feedbackCheck(); }
  } else { psDown = 0; psDone = false; }

  /* Keep the link fed so the pad stays in 0x31 mode. */
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 100) { lastSend = millis(); idleLed(); }

  delay(15);
}

#endif

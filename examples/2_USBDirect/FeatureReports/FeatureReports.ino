#include <ds_hci.h>
#include <ds_protocol.h>
#include <ds_usb_hid.h>

/*
 * FeatureReports — a DualSense plugged straight into an ESP32-S3.
 *
 * No radio and no pairing: the pad is a HID device on a wire. That also makes
 * it the place to ask it things, because feature reports are readable over the
 * control pipe:
 *
 *   0x09  its own Bluetooth address
 *   0x05  factory motion calibration
 *
 * The address is worth having even if you intend to use Bluetooth: it comes
 * out in the byte order the HCI layer wants, so a pad identified on a wire can
 * be paged over the air without hunting for it.
 *
 * Wiring on an ESP32-S3-DevKitC-1: the log goes out of the UART port, and the
 * pad hangs off the OTG port through a USB-C to USB-A adapter and the cable
 * the controller came with.
 *
 * Output is the same event log as the other examples:
 *
 *   [  2.14s] CROSS     down
 *   [  3.02s] LSTICK    (  42, -118)
 *
 *   OPTIONS        snapshot of everything right now
 *   PS (hold 1 s)  feedback check: lightbar, rumble, all four trigger effects
 */

#if !defined(DS_HAS_USB_HID)

void setup() { Serial.begin(115200); }
void loop() {
  Serial.println("Needs USB OTG: use an ESP32-S3 or S2.");
  delay(4000);
}

#else

static const int STICK_STEP = 12;
static const int TRIGGER_STEP = 16;
static const int TOUCH_STEP = 40;
/* Calibrated units: 1024 LSB per deg/s, so this is about 3 deg/s. The
 * Bluetooth examples use a much smaller number because their readings are
 * still raw, at roughly 16 LSB per deg/s. */
static const long GYRO_WAKE = 3000;

static const uint8_t IDLE_R = 0, IDLE_G = 90, IDLE_B = 140;

static ds_state_t st;
static ds_calibration_t cal;
static uint8_t padAddr[6];
static bool haveAddr = false;
static uint8_t trigL[11], trigR[11];
static uint32_t reports = 0;

static void stamp() { Serial.printf("[%7.2fs] ", millis() / 1000.0f); }

/* ------------------------------------------------------------------ pump -- */

/* Everything is polled from loop(), so a plain delay() stops the pad talking. */
static void pumpDelay(uint32_t ms) {
  uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    ds_usb_hid_poll();
    delay(1);
  }
}

/* ---------------------------------------------------------------- output -- */

/* USB output is report 0x02 and carries no CRC and no sequence tag — both are
 * Bluetooth's problem. Same builder, different link. */
static void send(uint8_t r, uint8_t g, uint8_t b, uint8_t rl, uint8_t rr,
                 uint8_t leds) {
  ds_output_t out;
  uint8_t buf[DS_OUT_MAX_SIZE];
  uint16_t n;

  ds_output_init(&out);
  out.set_lightbar = true;    out.red = r; out.green = g; out.blue = b;
  out.set_rumble = true;      out.rumble_left = rl; out.rumble_right = rr;
  out.set_player_leds = true; out.player_leds = leds;
  out.set_trigger_left = true;  memcpy(out.trigger_left, trigL, 11);
  out.set_trigger_right = true; memcpy(out.trigger_right, trigR, 11);

  n = ds_build_output(&out, DS_LINK_USB, buf, sizeof(buf));
  if (n) ds_usb_hid_send(buf, n);
}

static void idleLed() { send(IDLE_R, IDLE_G, IDLE_B, 0, 0, 0); }

/* --------------------------------------------------------------- buttons -- */

static const char* BTN[] = {
  "UP", "RIGHT", "DOWN", "LEFT", "SQUARE", "CROSS", "CIRCLE", "TRIANGLE",
  "L1", "R1", "L2", "R2", "CREATE", "OPTIONS", "L3", "R3", "PS", "TOUCHPAD",
  "MUTE",
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
  static int l2v = 0, r2v = 0;

  int n = st.analog.l2;
  if (abs(n - l2v) >= TRIGGER_STEP || (n == 0) != (l2v == 0)) {
    l2v = n;
    stamp(); Serial.printf("%-9s %4d\n", "L2", l2v);
  }
  n = st.analog.r2;
  if (abs(n - r2v) >= TRIGGER_STEP || (n == 0) != (r2v == 0)) {
    r2v = n;
    stamp(); Serial.printf("%-9s %4d\n", "R2", r2v);
  }
}

static void logTouch() {
  static bool was = false;
  static int lastX = 0, lastY = 0;

  bool now = st.touch[0].active;
  int x = st.touch[0].x, y = st.touch[0].y;
  if (now && !was) {
    stamp();
    Serial.printf("%-9s down  id%-3u (%4d,%5d)\n", "TOUCH", st.touch[0].id, x, y);
    lastX = x; lastY = y;
  } else if (now && (abs(x - lastX) >= TOUCH_STEP || abs(y - lastY) >= TOUCH_STEP)) {
    stamp(); Serial.printf("%-9s move       (%4d,%5d)\n", "TOUCH", x, y);
    lastX = x; lastY = y;
  } else if (!now && was) {
    stamp(); Serial.printf("%-9s up\n", "TOUCH");
  }
  was = now;
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
  Serial.printf("%-9s gyro(%7ld,%7ld,%7ld)  |a|=%ld\n", "MOTION", gx, gy, gz,
                accelMag());
}

static void logStatus() {
  static uint8_t lastReport = 0, lastBat = 255, lastChg = 255;

  if (st.report_id != lastReport) {
    lastReport = st.report_id;
    stamp();
    Serial.printf("%-9s 0x%02X  (usb, %u bytes)\n", "REPORT", lastReport,
                  DS5_IN_USB_SIZE);
  }
  if (st.status.valid &&
      (st.status.battery != lastBat || st.status.charging != lastChg)) {
    lastBat = st.status.battery;
    lastChg = st.status.charging;
    stamp();
    Serial.printf("%-9s %u%s\n", "BATTERY", lastBat,
                  lastChg == 1 ? "  charging" : (lastChg == 2 ? "  full" : ""));
  }
}

/* ----------------------------------------------------------- consistency -- */

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
         "accel magnitude far from 1 g -> motion offset or calibration" },
    { 5, st.report_id == DS5_IN_USB && !st.status.valid,
         "usb report without valid status -> status offset" },
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
  char s[18];
  Serial.println("--- snapshot ------------------------------------");
  if (haveAddr) {
    ds_hci_addr_to_str(padAddr, s);
    Serial.printf("  bluetooth address %s\n", s);
  }
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

  Serial.printf("  gyro (%7ld,%7ld,%7ld)   accel (%5d,%5d,%5d)  |a|=%ld%s\n",
                (long)st.motion.gyro[0], (long)st.motion.gyro[1],
                (long)st.motion.gyro[2],
                st.motion.accel[0], st.motion.accel[1], st.motion.accel[2],
                accelMag(), cal.valid ? "  (calibrated)" : "  (raw)");
  Serial.printf("  touch1 %s (%4u,%4u) id%-3u   touch2 %s (%4u,%4u)\n",
                st.touch[0].active ? "down" : "up  ", st.touch[0].x,
                st.touch[0].y, st.touch[0].id,
                st.touch[1].active ? "down" : "up  ", st.touch[1].x,
                st.touch[1].y);
  Serial.printf("  %lu reports\n", (unsigned long)reports);
  Serial.println("-------------------------------------------------");
}

/* -------------------------------------------------------- feedback check -- */

static void feedbackCheck() {
  Serial.println("=== FEEDBACK CHECK ==============================");

  Serial.println("  lightbar RED");   send(255, 0, 0, 0, 0, 0x01); pumpDelay(700);
  Serial.println("  lightbar GREEN"); send(0, 255, 0, 0, 0, 0x02); pumpDelay(700);
  Serial.println("  lightbar BLUE");  send(0, 0, 255, 0, 0, 0x04); pumpDelay(700);
  Serial.println("  rumble weak");    send(0, 0, 0, 90, 0, 0x08);  pumpDelay(600);
  Serial.println("  rumble strong");  send(0, 0, 0, 0, 200, 0x10); pumpDelay(600);
  send(0, 0, 0, 0, 0, 0);

  Serial.println("  trigger FEEDBACK   squeeze L2/R2 - steady resistance");
  ds5_trigger_feedback(trigL, 3, 6); ds5_trigger_feedback(trigR, 3, 6);
  send(0, 60, 60, 0, 0, 0); pumpDelay(2500);

  Serial.println("  trigger WEAPON     squeeze - should LOCK then break");
  ds5_trigger_weapon(trigL, 3, 6, 8); ds5_trigger_weapon(trigR, 3, 6, 8);
  send(60, 60, 0, 0, 0, 0); pumpDelay(2500);

  Serial.println("  trigger VIBRATION  squeeze - should buzz");
  ds5_trigger_vibration(trigL, 2, 6, 30); ds5_trigger_vibration(trigR, 2, 6, 30);
  send(60, 0, 60, 0, 0, 0); pumpDelay(2500);

  ds5_trigger_off(trigL); ds5_trigger_off(trigR);
  idleLed();
  Serial.println("=== done - lightbar back to idle colour ==========");
}

/* ------------------------------------------------------ feature reports -- */

static void readFeatures() {
  uint8_t buf[DS5_FEATURE_CALIBRATION_SIZE];
  char s[18];
  int n;

  n = ds_usb_hid_get_feature(DS5_FEATURE_PAIRING_INFO, buf,
                              DS5_FEATURE_PAIRING_INFO_SIZE, 500);
  if (n > 0 && ds_parse_pairing_info(buf, (uint16_t)n, padAddr)) {
    haveAddr = true;
    ds_hci_addr_to_str(padAddr, s);
    stamp();
    Serial.printf("      bluetooth address %s\n", s);
  } else {
    stamp();
    Serial.printf("      pairing info unreadable (%d)\n", n);
  }

  n = ds_usb_hid_get_feature(DS5_FEATURE_CALIBRATION, buf,
                              DS5_FEATURE_CALIBRATION_SIZE, 500);
  if (n > 0 && ds_parse_calibration(buf, (uint16_t)n, &cal)) {
    stamp();
    Serial.printf("      calibration: gyro x%ld/%ld  accel bias(%d,%d,%d)\n",
                  (long)cal.gyro[0].numer, (long)cal.gyro[0].denom,
                  cal.accel[0].bias, cal.accel[1].bias, cal.accel[2].bias);
  } else {
    stamp();
    Serial.printf("      calibration unreadable (%d) - motion stays raw\n", n);
  }
}

/* ------------------------------------------------------------------------- */

static void onReport(void* ctx, const uint8_t* report, uint16_t len) {
  ds_state_t tmp;
  (void)ctx;
  if (!ds_parse_input(report, len, DS_LINK_USB, &tmp)) return;
  ds_apply_calibration(&cal, &tmp.motion);
  st = tmp;
  reports++;
}

static void onAttach(void* ctx, uint16_t vid, uint16_t pid) {
  (void)ctx;
  stamp();
  Serial.printf("[1] controller attached, %04X:%04X\n", vid, pid);
  readFeatures();
  stamp();
  Serial.println("[2] ready - press buttons");
  idleLed();
}

static void onDetach(void* ctx) {
  (void)ctx;
  stamp();
  Serial.println("LINK      unplugged");
  memset(&cal, 0, sizeof(cal));
  haveAddr = false;
}

static void logMsg(void* ctx, const char* msg) {
  (void)ctx;
  stamp();
  Serial.printf("      %s\n", msg);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("FeatureReports - DualSense on a wire");
  Serial.println("  OPTIONS       snapshot of everything");
  Serial.println("  PS (hold 1s)  feedback check (lightbar / rumble / triggers)");
  Serial.println();

  ds5_trigger_off(trigL);
  ds5_trigger_off(trigR);

  ds_usb_hid_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.on_report = onReport;
  cfg.on_attach = onAttach;
  cfg.on_detach = onDetach;
  cfg.log = logMsg;
  if (!ds_usb_hid_init(&cfg)) {
    Serial.println("usb host init failed");
    return;
  }
  Serial.println("plug the controller into the OTG port");
}

void loop() {
  ds_usb_hid_poll();
  if (!ds_usb_hid_attached() || reports == 0) return;

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

  static unsigned long lastSend = 0;
  if (millis() - lastSend > 100) { lastSend = millis(); idleLed(); }
}

#endif /* DS_HAS_USB_HID */

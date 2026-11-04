#include <Preferences.h>
#include <ds_hci_host.h>
#include <ds_l2cap.h>
#include <ds_protocol.h>
#include <ds_usb_hci.h>

/*
 * DongleStepByStep — a DualSense on an ESP32-S3, over a USB Bluetooth dongle.
 *
 * The S3 has no Bluetooth Classic radio, so the dongle is the radio and this
 * board is the whole host stack above it:
 *
 *     ds_usb_hci    USB pipes            <- the only part that knows about USB
 *     ds_hci_host   bring-up, pairing
 *     ds_l2cap      the two HID channels
 *     ds_protocol   reports in and out
 *
 * Hardware: an ESP32-S3-DevKitC-1 with the dongle in the OTG port and the log
 * coming out of the UART port, so the log survives.
 *
 * Bringing up the link prints numbered milestones, so a failure says where.
 * After that it is the same event log as EventLog:
 *
 *   [  2.14s] CROSS     down
 *   [  3.02s] LSTICK    (  42, -118)
 *   [  4.10s] TOUCH     down  id19 (1133,  809)
 *   [  6.60s] MOTION    gyro(  945,  -31, -232)  |a|=8186
 *
 *   OPTIONS        snapshot of everything right now
 *   PS (hold 1 s)  feedback check: lightbar, rumble, all four trigger effects
 *
 * FIRST TIME  put the DualSense in pairing mode: hold Create (left of the
 *             touchpad) and then hold PS until the bar flashes fast. The bond
 *             is written to NVS.
 * AFTER THAT  just press PS. The pad remembers us and calls in by itself, so
 *             there is nothing to scan for and nothing to page.
 *             Send 'c' over the serial port to forget the bond.
 *
 * NOTE the one thing that differs from the Bluetooth-Classic examples. There,
 * the radio stack runs in its own tasks and a delay() is harmless. Here every
 * pipe is driven from loop() by ds_usb_hci_poll(), so a plain delay() stops
 * the link dead: nothing is resubmitted and nothing is sent. Use pumpDelay().
 */

#if !defined(DS_HAS_USB_HCI)

void setup() { Serial.begin(115200); }
void loop() {
  Serial.println("Needs USB OTG: use an ESP32-S3 or S2.");
  delay(4000);
}

#else

/* How far an analog input must move before it is worth a line. */
static const int STICK_STEP = 12;
static const int TRIGGER_STEP = 16;
static const int TOUCH_STEP = 40;
/* Roughly 25 deg/s either way, but the units change once the factory
 * calibration lands: raw is about 16 LSB per deg/s and calibrated is 1024. */
static const long GYRO_WAKE_RAW = 400;
static const long GYRO_WAKE_CAL = 25000;

/* Resting lightbar colour, restored after the feedback check. */
static const uint8_t IDLE_R = 0, IDLE_G = 90, IDLE_B = 140;

static ds_hci_host_t host;
static ds_l2cap_t l2;
static ds_state_t st;
static ds_calibration_t cal;
static uint8_t seq;
static uint8_t trigL[11], trigR[11];

static bool haveKey = false;
static uint8_t keyAddr[DS_HCI_BD_ADDR_LEN];
static uint8_t keyValue[DS_HCI_LINK_KEY_LEN];
static bool connecting = false;
static uint32_t reports = 0, sendsOk = 0, sendsFailed = 0;

static void stamp() { Serial.printf("[%7.2fs] ", millis() / 1000.0f); }

/* ------------------------------------------------------------------ bond -- */

/* The address and link key of the pad we are bonded to, kept in NVS so the
 * pairing survives a reset. Storing the key is what makes the second and
 * every later connection a single press of PS: without it the pad has to be
 * put back into pairing mode every time. */
static Preferences prefs;
static const char* BOND_NS = "ds5bond";

static bool loadBond() {
  bool ok;
  prefs.begin(BOND_NS, true);
  ok = prefs.getBytesLength("addr") == DS_HCI_BD_ADDR_LEN &&
       prefs.getBytesLength("key") == DS_HCI_LINK_KEY_LEN;
  if (ok) {
    prefs.getBytes("addr", keyAddr, DS_HCI_BD_ADDR_LEN);
    prefs.getBytes("key", keyValue, DS_HCI_LINK_KEY_LEN);
  }
  prefs.end();
  return ok;
}

static void saveBond() {
  prefs.begin(BOND_NS, false);
  prefs.putBytes("addr", keyAddr, DS_HCI_BD_ADDR_LEN);
  prefs.putBytes("key", keyValue, DS_HCI_LINK_KEY_LEN);
  prefs.end();
}

static void clearBond() {
  prefs.begin(BOND_NS, false);
  prefs.clear();
  prefs.end();
  haveKey = false;
  stamp();
  Serial.println("      bond forgotten - pair again with Create + PS");
}

/* ------------------------------------------------------------------ pump -- */

/* A delay that keeps the USB pipes turning. Everything below this sketch is
 * polled, so stopping the poll stops the Bluetooth link too. */
static void pumpDelay(uint32_t ms) {
  uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    ds_usb_hci_poll();
    delay(1);
  }
}

/* ---------------------------------------------------------------- output -- */

static void send(uint8_t r, uint8_t g, uint8_t b, uint8_t rl, uint8_t rr,
                 uint8_t leds) {
  ds_output_t out;
  uint8_t buf[DS_OUT_MAX_SIZE];
  uint16_t n;

  if (!ds_l2cap_is_open(&l2, DS_L2CAP_PSM_HID_INTERRUPT)) return;

  ds_output_init(&out);
  out.set_lightbar = true;    out.red = r; out.green = g; out.blue = b;
  out.set_rumble = true;      out.rumble_left = rl; out.rumble_right = rr;
  out.set_player_leds = true; out.player_leds = leds;
  out.set_trigger_left = true;  memcpy(out.trigger_left, trigL, 11);
  out.set_trigger_right = true; memcpy(out.trigger_right, trigR, 11);
  out.sequence = seq++;   /* inside the CRC, so it is set before building */

  n = ds_build_output(&out, DS_LINK_BT, buf, sizeof(buf));
  if (n == 0) return;
  if (ds_l2cap_send(&l2, DS_L2CAP_PSM_HID_INTERRUPT, buf, n)) {
    sendsOk++;
  } else {
    sendsFailed++;
  }
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
  static bool was = false, was2 = false;
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
  long wake = cal.valid ? GYRO_WAKE_CAL : GYRO_WAKE_RAW;
  if (labs(gx) < wake && labs(gy) < wake && labs(gz) < wake) return;
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
    Serial.printf("%-9s 0x%02X%s\n", "REPORT", lastReport,
                  lastReport == 0x31 ? "  (full: battery/motion/touch)"
                                     : "  (compatibility: sticks+buttons only)");
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

  Serial.printf("  gyro (%7ld,%7ld,%7ld)   accel (%5d,%5d,%5d)  |a|=%ld%s\n",
                (long)st.motion.gyro[0], (long)st.motion.gyro[1],
                (long)st.motion.gyro[2],
                st.motion.accel[0], st.motion.accel[1], st.motion.accel[2],
                accelMag(),
                cal.valid ? "  (calibrated)" : "  (raw)");
  Serial.printf("  touch1 %s (%4u,%4u) id%-3u   touch2 %s (%4u,%4u)\n",
                st.touch[0].active ? "down" : "up  ", st.touch[0].x,
                st.touch[0].y, st.touch[0].id,
                st.touch[1].active ? "down" : "up  ", st.touch[1].x,
                st.touch[1].y);
  Serial.printf("  link: dongle %04X, acl credits %u/%u, l2cap %s\n",
                0x0012, host.acl_credits, host.acl_credits_total,
                ds_l2cap_hid_ready(&l2) ? "both channels open" : "incomplete");
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

/* ------------------------------------------------------------ layer wiring */

static void usbEvent(void* ctx, const uint8_t* p, uint16_t n) {
  (void)ctx;
  ds_hci_host_handle_event(&host, p, n);
}

static void usbAcl(void* ctx, const uint8_t* p, uint16_t n) {
  (void)ctx;
  ds_hci_host_handle_acl(&host, p, n);
}

static void usbAttach(void* ctx, uint16_t vid, uint16_t pid) {
  (void)ctx;
  stamp();
  Serial.printf("[1] dongle attached, %04X:%04X\n", vid, pid);
  ds_hci_host_start(&host);
}

static void usbDetach(void* ctx) {
  (void)ctx;
  stamp();
  Serial.println("LINK      dongle removed");
  ds_l2cap_reset(&l2);
  connecting = false;
}

static void hostL2cap(void* ctx, uint16_t cid, const uint8_t* d, uint16_t n) {
  (void)ctx;
  ds_l2cap_handle(&l2, cid, d, n);
}

static int l2capSend(void* ctx, uint16_t cid, const uint8_t* d, uint16_t n) {
  (void)ctx;
  return ds_hci_host_send_l2cap(&host, cid, d, n) ? 0 : -1;
}

/* ---------------------------------------------------------------- pairing */

static bool getKey(void* ctx, const uint8_t addr[6], uint8_t key[16]) {
  (void)ctx;
  if (!haveKey || memcmp(addr, keyAddr, DS_HCI_BD_ADDR_LEN) != 0) return false;
  memcpy(key, keyValue, DS_HCI_LINK_KEY_LEN);
  return true;
}

static void newKey(void* ctx, const uint8_t addr[6], const uint8_t key[16]) {
  (void)ctx;
  char s[18];
  memcpy(keyAddr, addr, DS_HCI_BD_ADDR_LEN);
  memcpy(keyValue, key, DS_HCI_LINK_KEY_LEN);
  haveKey = true;
  saveBond();
  ds_hci_addr_to_str(keyAddr, s);
  stamp();
  Serial.printf("      bonded to %s, saved - next time just press PS\n", s);
}

static void deviceFound(void* ctx, const uint8_t addr[6], uint32_t cod,
                        int8_t rssi, bool hasRssi) {
  char s[18];
  (void)ctx;
  if (!ds_hci_cod_is_gamepad(cod) || connecting) return;

  ds_hci_addr_to_str(addr, s);
  connecting = true;
  stamp();
  Serial.printf("[3] gamepad %s", s);
  if (hasRssi) Serial.printf(" at %d dBm", rssi);
  Serial.println(", connecting");
  ds_hci_host_connect(&host, addr);
}

static void hostState(void* ctx, ds_hci_host_state_t s) {
  (void)ctx;

  /* An ACL link that drops takes its L2CAP channels with it, but silently:
   * there is no disconnection signalling to hear, so the channel table has to
   * be told. Skipping this leaves channels that look open, and the next
   * connect_hid fails because those PSMs are already in use. */
  if (s != DS_HOST_CONNECTED && ds_l2cap_is_open(&l2, DS_L2CAP_PSM_HID_CONTROL)) {
    ds_l2cap_reset(&l2);
    connecting = false;
  }

  if (s == DS_HOST_READY || s == DS_HOST_FAILED) {
    static bool announced = false;
    connecting = false;

    if (!announced) {
      char a[18];
      announced = true;
      ds_hci_addr_to_str(host.local_addr, a);
      stamp();
      Serial.printf("[2] radio ready: hci %u, manufacturer %u, %s\n",
                    host.hci_version, host.manufacturer, a);
      stamp();
      if (haveKey) {
        char p[18];
        ds_hci_addr_to_str(keyAddr, p);
        Serial.printf("      bonded to %s - press PS\n", p);
      } else {
        Serial.println("      hold Create, then hold PS, until the bar flashes");
      }
    }
    if (s == DS_HOST_FAILED) {
      stamp();
      Serial.printf("LINK      attempt failed: %s (0x%02X)\n",
                    ds_hci_error_name(host.last_conn_status),
                    host.last_conn_status);
    }

    /* With a bond there is nothing to look for: the pad knows our address and
     * pages us when PS is pressed, and page scan is already on. Scanning here
     * would be worse than useless, because an inquiry stops the controller
     * answering pages — the pad would press PS into silence. */
    if (!haveKey) ds_hci_host_inquiry(&host, 8);
  } else if (s == DS_HOST_CONNECTING) {
    connecting = true;
  } else if (s == DS_HOST_CONNECTED) {
    stamp();
    /* Always from this end. A pad that paged us still waits to be asked, so
     * holding back only adds a delay to every reconnection. The L2CAP layer
     * copes if both ends ever do try at once. */
    Serial.printf("[4] link encrypted (%s), opening HID channels\n",
                  host.inbound ? "pad called in" : "we paged");
    ds_l2cap_connect_hid(&l2);
  }
}

static void chanOpen(void* ctx, uint16_t psm) {
  (void)ctx;

  if (psm == DS_L2CAP_PSM_HID_CONTROL) {
    /* Feature reports live on the control channel. Asking now means the
     * calibration is usually in before the first motion report. */
    uint8_t req[2];
    uint16_t n = ds_build_bt_feature_request(DS5_FEATURE_CALIBRATION, req,
                                              sizeof(req));
    if (n) ds_l2cap_send(&l2, DS_L2CAP_PSM_HID_CONTROL, req, n);
    return;
  }
  if (psm != DS_L2CAP_PSM_HID_INTERRUPT) return;
  stamp();
  Serial.println("[5] LINK      CONNECTED");
  Serial.println("      OPTIONS = snapshot,  PS held 1 s = feedback check");
  idleLed();
}

static void chanClose(void* ctx, uint16_t psm) {
  (void)ctx;
  /* The next pad to connect may not be this one. */
  memset(&cal, 0, sizeof(cal));
  if (psm != DS_L2CAP_PSM_HID_INTERRUPT) return;
  stamp();
  Serial.printf("LINK      disconnected: %s (0x%02X)\n",
                ds_hci_error_name(host.last_disconnect_reason),
                host.last_disconnect_reason);
}

static void hidData(void* ctx, uint16_t psm, const uint8_t* d, uint16_t n) {
  const uint8_t* report;
  uint16_t rlen = 0;
  ds_state_t tmp;
  (void)ctx;

  if (psm == DS_L2CAP_PSM_HID_CONTROL) {
    /* The only thing we ever ask for is the calibration. A report that fails
     * its CRC comes back as NULL and is simply not used, which leaves the
     * motion raw rather than scaled by something corrupt. */
    report = ds_find_bt_feature(d, n, &rlen);
    if (report == NULL || report[0] != DS5_FEATURE_CALIBRATION) return;
    if (!ds_parse_calibration(report, rlen, &cal)) return;
    stamp();
    Serial.printf("      calibration over bluetooth: gyro x%ld/%ld\n",
                  (long)cal.gyro[0].numer, (long)cal.gyro[0].denom);
    return;
  }

  if (psm != DS_L2CAP_PSM_HID_INTERRUPT) return;
  report = ds_find_bt_report(d, n, &rlen);
  if (report == NULL) return;
  if (!ds_parse_input(report, rlen, DS_LINK_BT, &tmp)) return;
  ds_apply_calibration(&cal, &tmp.motion);
  st = tmp;
  reports++;
}

static void logMsg(void* ctx, const char* msg) {
  (void)ctx;
  stamp();
  Serial.printf("      %s\n", msg);
}

/* ------------------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("DongleStepByStep - DualSense over a USB Bluetooth dongle");
  Serial.println("  OPTIONS       snapshot of everything");
  Serial.println("  PS (hold 1s)  feedback check (lightbar / rumble / triggers)");
  Serial.println();

  ds5_trigger_off(trigL);
  ds5_trigger_off(trigR);

  haveKey = loadBond();
  if (haveKey) {
    char s[18];
    ds_hci_addr_to_str(keyAddr, s);
    Serial.printf("bonded to %s already - press PS ('c' to forget)\n", s);
  } else {
    Serial.println("not bonded yet - hold Create, then PS, to pair");
  }

  ds_hci_host_config_t hcfg;
  memset(&hcfg, 0, sizeof(hcfg));
  hcfg.send_cmd = ds_usb_hci_send_cmd;
  hcfg.send_acl = ds_usb_hci_send_acl;
  hcfg.on_state = hostState;
  hcfg.on_l2cap = hostL2cap;
  hcfg.get_link_key = getKey;
  hcfg.on_link_key = newKey;
  hcfg.on_device_found = deviceFound;
  hcfg.log = logMsg;
  if (!ds_hci_host_init(&host, &hcfg)) {
    Serial.println("hci host init failed");
    return;
  }

  ds_l2cap_config_t lcfg;
  memset(&lcfg, 0, sizeof(lcfg));
  lcfg.send = l2capSend;
  lcfg.on_open = chanOpen;
  lcfg.on_close = chanClose;
  lcfg.on_data = hidData;
  lcfg.log = logMsg;
  if (!ds_l2cap_init(&l2, &lcfg)) {
    Serial.println("l2cap init failed");
    return;
  }

  ds_usb_hci_config_t ucfg;
  memset(&ucfg, 0, sizeof(ucfg));
  ucfg.on_event = usbEvent;
  ucfg.on_acl = usbAcl;
  ucfg.on_attach = usbAttach;
  ucfg.on_detach = usbDetach;
  ucfg.log = logMsg;
  if (!ds_usb_hci_init(&ucfg)) {
    Serial.println("usb host init failed");
    return;
  }
  Serial.println("waiting for the dongle on the OTG port");
}

void loop() {
  ds_usb_hci_poll();

  if (Serial.available() && Serial.read() == 'c') clearBond();

  if (!ds_l2cap_is_open(&l2, DS_L2CAP_PSM_HID_INTERRUPT)) return;

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

  /* Five seconds of traffic in one line. A link that is about to time out
   * usually stops moving one of these numbers first, and which one it is
   * says whether the trouble is inbound, outbound or neither. */
  static unsigned long lastBeat = 0;
  static uint32_t lastReports = 0;
  if (millis() - lastBeat > 5000) {
    lastBeat = millis();
    stamp();
    Serial.printf("PULSE     in %lu (+%lu)  out ok %lu fail %lu  credits %u/%u\n",
                  (unsigned long)reports,
                  (unsigned long)(reports - lastReports),
                  (unsigned long)sendsOk, (unsigned long)sendsFailed,
                  host.acl_credits, host.acl_credits_total);
    lastReports = reports;
  }
}

#endif /* DS_HAS_USB_HCI */

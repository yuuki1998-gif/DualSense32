#include <ds_protocol.h>

#include "vectors.h"

/*
 * ProtocolCore — validates the transport-agnostic protocol core.
 *
 * No controller, no Bluetooth, no USB: this only checks that the core turns
 * parameters into the exact bytes a DualSense expects, and turns real reports
 * back into the right state. The vectors in vectors.h are generated from the
 * reference sources (Linux hid-playstation, Nielk1's trigger generator, zlib
 * CRC-32), not from this implementation, so a transcription slip shows up.
 *
 * Run this on any ESP32 before wiring up a transport.
 */

static int passCount = 0, failCount = 0;

static void check(const char* name, bool ok) {
  if (ok) { passCount++; return; }
  failCount++;
  Serial.printf("  FAIL  %s\n", name);
}

static void checkBytes(const char* name, const uint8_t* got, const uint8_t* want,
                       size_t n) {
  if (memcmp(got, want, n) == 0) { passCount++; return; }
  failCount++;
  Serial.printf("  FAIL  %s\n   got  ", name);
  for (size_t i = 0; i < n; i++) Serial.printf("%02X ", got[i]);
  Serial.print("\n   want ");
  for (size_t i = 0; i < n; i++) Serial.printf("%02X ", want[i]);
  Serial.println();
}

/* ---------------------------------------------------------------- triggers */

static void testTriggers() {
  Serial.println("triggers");
  uint8_t b[DS5_TRIGGER_BLOCK_SIZE];

  ds5_trigger_off(b);
  checkBytes("off", b, V_TRIG_OFF, sizeof(b));

  check("feedback(3,6) accepted", ds5_trigger_feedback(b, 3, 6));
  checkBytes("feedback(3,6)", b, V_TRIG_FB_3_6, sizeof(b));

  check("weapon(3,6,8) accepted", ds5_trigger_weapon(b, 3, 6, 8));
  checkBytes("weapon(3,6,8)", b, V_TRIG_WPN_3_6_8, sizeof(b));

  check("vibration(2,6,30) accepted", ds5_trigger_vibration(b, 2, 6, 30));
  checkBytes("vibration(2,6,30)", b, V_TRIG_VIB_2_6_30, sizeof(b));

  /*
   * The general forms. Feedback and Vibration above are these with every
   * zone holding the same number, which is worth seeing written out: a
   * shaped resistance is not a different feature, only a different list.
   */
  {
    static const uint8_t strength[10] = {0, 0, 3, 3, 6, 6, 8, 0, 0, 1};
    check("multi feedback accepted", ds5_trigger_multi_feedback(b, strength));
    checkBytes("multi feedback", b, V_TRIG_MULTI_FB, sizeof(b));
  }
  {
    static const uint8_t amplitude[10] = {0, 2, 4, 6, 8, 0, 0, 0, 0, 5};
    check("multi vibration accepted",
          ds5_trigger_multi_vibration(b, 35, amplitude));
    checkBytes("multi vibration", b, V_TRIG_MULTI_VIB, sizeof(b));
  }

  check("slope(1,8,2,7) accepted", ds5_trigger_slope(b, 1, 8, 2, 7));
  checkBytes("slope(1,8,2,7)", b, V_TRIG_SLOPE_1_8_2_7, sizeof(b));

  /* The three Sony never documented. */
  check("bow(1,5,6,7) accepted", ds5_trigger_bow(b, 1, 5, 6, 7));
  checkBytes("bow(1,5,6,7)", b, V_TRIG_BOW_1_5_6_7, sizeof(b));

  check("galloping(1,8,2,5,20) accepted",
        ds5_trigger_galloping(b, 1, 8, 2, 5, 20));
  checkBytes("galloping(1,8,2,5,20)", b, V_TRIG_GALLOP_1_8_2_5_20, sizeof(b));

  check("machine(1,9,3,6,25,4) accepted",
        ds5_trigger_machine(b, 1, 9, 3, 6, 25, 4));
  checkBytes("machine(1,9,3,6,25,4)", b, V_TRIG_MACHINE_1_9_3_6_25_4,
             sizeof(b));

  /* Out of range must be rejected and fall back to Off, not emit junk. */
  check("weapon(1,6,8) rejected", !ds5_trigger_weapon(b, 1, 6, 8));
  checkBytes("weapon(1,6,8) -> off", b, V_TRIG_OFF, sizeof(b));

  /* The undocumented three have their own limits, and a controller handed a
   * value outside them does something arbitrary rather than refusing. */
  check("bow needs end above start", !ds5_trigger_bow(b, 5, 5, 6, 7));
  checkBytes("bad bow -> off", b, V_TRIG_OFF, sizeof(b));
  check("galloping feet must be ordered",
        !ds5_trigger_galloping(b, 1, 8, 5, 2, 20));
  checkBytes("bad galloping -> off", b, V_TRIG_OFF, sizeof(b));
  check("machine amplitude stops at seven",
        !ds5_trigger_machine(b, 1, 9, 8, 6, 25, 4));
  checkBytes("bad machine -> off", b, V_TRIG_OFF, sizeof(b));
  check("slope needs a real span", !ds5_trigger_slope(b, 4, 4, 2, 7));
  checkBytes("bad slope -> off", b, V_TRIG_OFF, sizeof(b));
  {
    static const uint8_t none[10] = {0};
    check("an empty list is nothing to do",
          !ds5_trigger_multi_feedback(b, none));
    checkBytes("empty list -> off", b, V_TRIG_OFF, sizeof(b));
  }
  check("feedback(3,0) rejected", !ds5_trigger_feedback(b, 3, 0));
  checkBytes("feedback(3,0) -> off", b, V_TRIG_OFF, sizeof(b));
  check("vibration(2,6,0) rejected", !ds5_trigger_vibration(b, 2, 6, 0));
  check("feedback(10,4) rejected", !ds5_trigger_feedback(b, 10, 4));
}

/* ------------------------------------------------------------------ output */

static void testOutput() {
  Serial.println("output reports");
  uint8_t buf[DS_OUT_MAX_SIZE];
  ds_output_t out;

  ds_output_init(&out);
  out.set_lightbar = true;
  out.red = 10; out.green = 20; out.blue = 30;
  uint16_t n = ds_build_output(&out, DS_LINK_BT, buf, sizeof(buf));
  check("BT length 79", n == sizeof(V_OUT_BT_LED));
  if (n == sizeof(V_OUT_BT_LED)) checkBytes("BT lightbar only", buf, V_OUT_BT_LED, n);

  ds_output_init(&out);
  out.set_rumble = true;  out.rumble_left = 200; out.rumble_right = 100;
  out.set_lightbar = true; out.red = 1; out.green = 2; out.blue = 3;
  out.set_player_leds = true; out.player_leds = 0x04;
  out.set_trigger_right = true; ds5_trigger_weapon(out.trigger_right, 3, 6, 8);
  out.set_trigger_left = true;  ds5_trigger_feedback(out.trigger_left, 3, 6);

  n = ds_build_output(&out, DS_LINK_BT, buf, sizeof(buf));
  check("BT length 79 (full)", n == sizeof(V_OUT_BT_FULL));
  if (n == sizeof(V_OUT_BT_FULL)) checkBytes("BT everything", buf, V_OUT_BT_FULL, n);

  n = ds_build_output(&out, DS_LINK_USB, buf, sizeof(buf));
  check("USB length 63", n == sizeof(V_OUT_USB_FULL));
  if (n == sizeof(V_OUT_USB_FULL)) checkBytes("USB everything", buf, V_OUT_USB_FULL, n);

  /* A short buffer must be refused, not overrun. */
  check("short buffer refused", ds_build_output(&out, DS_LINK_BT, buf, 10) == 0);

  /* The sequence tag sits inside the CRC-covered region, so bumping it has to
   * change both the tag byte and the checksum. Stamping it into an already
   * built buffer would invalidate the CRC and the controller would silently
   * drop every frame after the first. */
  uint8_t seqA[DS_OUT_MAX_SIZE], seqB[DS_OUT_MAX_SIZE];
  ds_output_init(&out);
  out.set_lightbar = true;
  out.red = 9;
  out.sequence = 0;
  ds_build_output(&out, DS_LINK_BT, seqA, sizeof(seqA));
  out.sequence = 5;
  ds_build_output(&out, DS_LINK_BT, seqB, sizeof(seqB));
  check("seq tag in high nibble", seqA[2] == 0x00 && seqB[2] == 0x50);
  check("seq is covered by crc", memcmp(seqA + 75, seqB + 75, 4) != 0);
  out.sequence = 16;
  ds_build_output(&out, DS_LINK_BT, seqB, sizeof(seqB));
  check("seq wraps at 4 bits", seqB[2] == 0x00);
  check("USB ignores seq",
        ds_build_output(&out, DS_LINK_USB, seqB, sizeof(seqB)) == DS5_OUT_USB_SIZE);
}

/* ------------------------------------------------------------------- input */

/* A real 0x31 frame captured from hardware: sticks near centre, hat neutral,
 * gyro at rest, accel ~1 g, both touch points lifted, battery 10. */
static const uint8_t REAL_BT_31[] = {
  0x31, 0x11, 0x81, 0x7D, 0x81, 0x7E, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00,
  0x00, 0x58, 0xCA, 0x22, 0x54, 0x12, 0x00, 0x8C, 0xFE, 0x2A, 0xFF, 0x5C,
  0x14, 0x3F, 0x16, 0xC8, 0xF4, 0x42, 0x7B, 0xA7, 0x00, 0x15, 0x80, 0x00,
  0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x5D, 0x8A, 0xA7, 0x00, 0x1A, 0x00, 0x00, 0x08, 0xDD, 0x5F,
  0x70, 0x2D, 0x27, 0x84, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x54, 0x5D, 0x83, 0x86,
};

static void testInput() {
  Serial.println("input reports");
  ds_state_t s;

  check("real 0x31 parsed", ds_parse_input(REAL_BT_31, sizeof(REAL_BT_31),
                                            DS_LINK_BT, &s));
  check("report id 0x31", s.report_id == 0x31);
  check("LX 0x81", s.analog.lx == 0x81);
  check("LY 0x7D", s.analog.ly == 0x7D);
  check("triggers released", s.analog.l2 == 0 && s.analog.r2 == 0);
  check("hat neutral", !s.button.up && !s.button.down && !s.button.left && !s.button.right);
  check("no face buttons", !s.button.cross && !s.button.square);
  check("motion present", s.has_motion);
  check("gyro x 0x0012", s.motion.gyro[0] == 0x0012);
  check("touch present", s.has_touch);
  check("touch1 lifted", !s.touch[0].active);
  check("touch2 lifted", !s.touch[1].active);
  /* status[0] = 0x1A -> battery 10, charging code 1 */
  check("battery valid", s.status.valid);
  check("battery 10", s.status.battery == 10);
  check("charging", s.status.charging == 1);

  /* Compatibility report: DS4 field order, buttons before triggers. */
  /* btn0 0x22 = hat 2 (east) + cross; btn1 0x02 = R1. */
  const uint8_t compat[] = { 0x01, 0x81, 0x7D, 0x80, 0x80, 0x22, 0x02, 0x00, 0x11, 0x22 };
  check("0x01 parsed", ds_parse_input(compat, sizeof(compat), DS_LINK_BT, &s));
  check("0x01 cross", s.button.cross);
  check("0x01 hat E -> right", s.button.right && !s.button.up);
  check("0x01 r1", s.button.r1);
  check("0x01 L2 analog 0x11", s.analog.l2 == 0x11);
  check("0x01 R2 analog 0x22", s.analog.r2 == 0x22);
  check("0x01 has no motion", !s.has_motion);
  check("0x01 has no touch", !s.has_touch);
  check("0x01 status invalid", !s.status.valid);

  /* Rejections: unknown ids, truncated frames, wrong link. */
  const uint8_t bogus[] = { 0x99, 0x00, 0x00, 0x00 };
  check("unknown id rejected", !ds_parse_input(bogus, sizeof(bogus), DS_LINK_BT, &s));
  check("short 0x01 rejected", !ds_parse_input(compat, 4, DS_LINK_BT, &s));
  check("0x31 rejected on USB link",
        !ds_parse_input(REAL_BT_31, sizeof(REAL_BT_31), DS_LINK_USB, &s));

  /* L2CAP payload framing. */
  uint8_t payload[1 + sizeof(REAL_BT_31)];
  payload[0] = 0xA1;
  memcpy(payload + 1, REAL_BT_31, sizeof(REAL_BT_31));
  uint16_t len = 0;
  const uint8_t* rep = ds_find_bt_report(payload, sizeof(payload), &len);
  check("framing finds report", rep == payload + 1);
  check("framing length", len == sizeof(REAL_BT_31));
  payload[0] = 0x00;
  check("framing rejects bad header",
        ds_find_bt_report(payload, sizeof(payload), &len) == NULL);
}

/* --------------------------------------------------------------------- crc */

static void testCrc() {
  Serial.println("crc32");
  /* Known zlib CRC-32 values. */
  const uint8_t abc[] = { 'a', 'b', 'c' };
  check("crc32(\"abc\")", ds_crc32(0, abc, 3) == 0x352441C2);
  check("crc32(empty)", ds_crc32(0, abc, 0) == 0x00000000);
  /* Chaining must equal one pass over the concatenation. */
  const uint8_t all[] = { 1, 2, 3, 4, 5, 6 };
  uint32_t chained = ds_crc32(ds_crc32(0, all, 2), all + 2, 4);
  check("crc32 chaining", chained == ds_crc32(0, all, 6));
}

static void runAll() {
  passCount = failCount = 0;
  Serial.println();
  Serial.println("=== DualSense32 core self-test ===");
  testCrc();
  testTriggers();
  testOutput();
  testInput();
  Serial.println();
  Serial.printf("%d passed, %d failed  ->  %s\n", passCount, failCount,
                failCount == 0 ? "PASS" : "*** FAILURES");
}

void setup() {
  Serial.begin(115200);
  delay(400);
  runAll();
}

/* Repeats every few seconds. Boards with native USB (ESP32-S3/C3) re-enumerate
 * on reset, so a one-shot print in setup() is usually gone before a serial
 * monitor can attach; re-running keeps the result reachable. */
void loop() {
  delay(4000);
  runAll();
}

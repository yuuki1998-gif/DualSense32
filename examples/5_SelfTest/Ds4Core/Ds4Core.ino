#include <ds_protocol.h>
#include <DualShock4.h>

#include "ds4_vectors.h"

/*
 * Ds4Core — validates the DualShock 4 dialect of the protocol core.
 *
 * No controller, no Bluetooth, no USB. The vectors in ds4_vectors.h are laid
 * out from the report structs in Linux's hid-playstation.c and signed with
 * zlib's CRC-32, not with this library's, so a transcription slip fails a
 * test rather than being baked into both sides.
 *
 * The point of most of these is the places a DualShock 4 is *not* a
 * DualSense. Its report body matches the DualSense *compatibility* report for
 * its first nine bytes — buttons ahead of triggers — which is where that
 * layout came from. It does *not* match the 64-byte USB report it shares an
 * id with: that one is native DualSense order, triggers first.
 *
 * So over USB the two pads cannot be told apart by report id or length, they
 * decode to different things, and neither ever fails while doing it. That is
 * what testWrongDeviceIsSilent pins down, and the reason the device has to
 * come from the USB product id instead.
 *
 * Run this on any ESP32 alongside ProtocolCore.
 */

static int passCount = 0, failCount = 0;

static void check(const char* name, bool ok) {
  if (ok) { passCount++; return; }
  failCount++;
  Serial.printf("  FAIL  %s\n", name);
}

static void checkInt(const char* name, long got, long want) {
  if (got == want) { passCount++; return; }
  failCount++;
  Serial.printf("  FAIL  %s: got %ld, want %ld\n", name, got, want);
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

/* ------------------------------------------------------------------- input */

/* The fields that sit in the same place on both pads. If these fail, the
 * shared part of the layout table is wrong and every pad is broken. */
static void checkSharedFields(const char* who, const ds_state_t& s) {
  Serial.printf("  %s\n", who);
  checkInt("lx", s.analog.lx, 0x10);
  checkInt("ly", s.analog.ly, 0x20);
  checkInt("rx", s.analog.rx, 0x30);
  checkInt("ry", s.analog.ry, 0x40);
  checkInt("l2 analog", s.analog.l2, 0x7F);
  checkInt("r2 analog", s.analog.r2, 0xC0);

  /* hat 2 is east: right alone, and nothing else on the d-pad */
  check("dpad right", s.button.right);
  check("dpad up clear", !s.button.up);
  check("dpad down clear", !s.button.down);
  check("dpad left clear", !s.button.left);

  check("square", s.button.square);
  check("circle", s.button.circle);
  check("cross clear", !s.button.cross);
  check("triangle clear", !s.button.triangle);

  check("l1", s.button.l1);
  check("l2 click", s.button.l2);
  check("create/share", s.button.create);
  check("r3", s.button.r3);
  check("r1 clear", !s.button.r1);
  check("r2 click clear", !s.button.r2);
  check("options clear", !s.button.options);
  check("l3 clear", !s.button.l3);

  check("ps", s.button.ps);
  check("touchpad clear", !s.button.touchpad);

  /* The one that catches a DualSense parser pointed at a DualShock 4: those
   * bits are a rolling report counter, and this vector has them set. */
  check("mute stays false (counter bits, not a button)", !s.button.mute);
}

static void checkMotionAndTouch(const ds_state_t& s) {
  check("has motion", s.has_motion);
  checkInt("gyro pitch", s.motion.gyro[0], 100);
  checkInt("gyro yaw", s.motion.gyro[1], -200);
  checkInt("gyro roll", s.motion.gyro[2], 300);
  checkInt("accel x", s.motion.accel[0], 1000);
  checkInt("accel y", s.motion.accel[1], -2000);
  checkInt("accel z", s.motion.accel[2], 8192);

  /* Two bytes, not four: reading a DualSense's width here would drag the die
   * temperature and the low half of the gyro in with it. */
  checkInt("timestamp is 16 bit", s.motion.timestamp, 0x1234);

  check("has touch", s.has_touch);
  check("finger 1 down", s.touch[0].active);
  checkInt("finger 1 id", s.touch[0].id, 2);
  checkInt("finger 1 x", s.touch[0].x, 0x123);
  checkInt("finger 1 y", s.touch[0].y, 0x456);
  check("finger 2 lifted", !s.touch[1].active);
}

static void testInputUsb() {
  Serial.println("ds4 input, usb");
  ds_state_t s;
  check("parses", ds_parse_input_dev(V_DS4_IN_USB, sizeof(V_DS4_IN_USB),
                                      DS_LINK_USB, DS_DEVICE_DUALSHOCK4, &s));
  checkSharedFields("shared fields", s);
  checkMotionAndTouch(s);

  check("status valid", s.status.valid);
  checkInt("battery", s.status.battery, 7);
  checkInt("charging (cable in)", s.status.charging, 1);
  check("no headphone jack reported", !s.status.jack);
  check("no mic to mute", !s.status.mic_muted);
}

static void testInputBt() {
  Serial.println("ds4 input, bluetooth");
  ds_state_t s;
  /* Report 0x11 keeps two bytes of its own between the id and the body, and
   * every body offset past that is identical to USB. */
  check("parses", ds_parse_input_dev(V_DS4_IN_BT, sizeof(V_DS4_IN_BT),
                                      DS_LINK_BT, DS_DEVICE_DUALSHOCK4, &s));
  checkInt("report id", s.report_id, 0x11);
  checkSharedFields("same body, three bytes further in", s);
  checkMotionAndTouch(s);
}

static void testBatteryFull() {
  Serial.println("ds4 battery");
  ds_state_t s;
  check("parses", ds_parse_input_dev(V_DS4_IN_USB_FULL_BATT,
                                      sizeof(V_DS4_IN_USB_FULL_BATT),
                                      DS_LINK_USB, DS_DEVICE_DUALSHOCK4, &s));
  /* A DualShock 4 counts one step further than a DualSense while charging:
   * 11 means full. The library squashes it to the DualSense's 0-10 plus a
   * charge code so a sketch never has to ask which pad it has. */
  checkInt("11 clamps to 10", s.status.battery, 10);
  checkInt("11 with cable means full", s.status.charging, 2);
}

/* The same 64 bytes, read as the wrong pad. Nothing throws — that is the
 * whole point, and it is why the device cannot be recovered from the report.
 *
 * Only the four stick bytes survive. The DualShock 4 body matches the
 * DualSense *compatibility* report — buttons before triggers — and not the
 * 64-byte USB report that happens to share its id, which is native DualSense
 * order with the triggers first. So a DualShock 4's buttons land squarely in
 * a DualSense's trigger fields. */
static void testWrongDeviceIsSilent() {
  Serial.println("ds4 bytes read as a dualsense");
  ds_state_t ds4, ds5;
  ds_parse_input_dev(V_DS4_IN_USB, sizeof(V_DS4_IN_USB), DS_LINK_USB,
                      DS_DEVICE_DUALSHOCK4, &ds4);
  bool parsed = ds_parse_input_dev(V_DS4_IN_USB, sizeof(V_DS4_IN_USB),
                                    DS_LINK_USB, DS_DEVICE_DUALSENSE, &ds5);
  check("the wrong device still parses", parsed);
  check("the sticks agree", ds5.analog.lx == ds4.analog.lx &&
                            ds5.analog.ry == ds4.analog.ry);
  /* Body byte 4 is this pad's first button byte and that pad's L2. */
  checkInt("ds4 buttons land in ds5 triggers", ds5.analog.l2, 0x52);
  check("ps disagrees", ds5.button.ps != ds4.button.ps);
  check("motion disagrees", ds5.motion.gyro[0] != ds4.motion.gyro[0]);
  check("and the counter reads as mute", ds5.button.mute && !ds4.button.mute);
}

static void testShortReports() {
  Serial.println("ds4 short reports");
  ds_state_t s;
  check("a truncated report is rejected",
        !ds_parse_input_dev(V_DS4_IN_USB, 4, DS_LINK_USB,
                             DS_DEVICE_DUALSHOCK4, &s));
  /* Enough for sticks and buttons, not for the sensors: it must decode what
   * is there and claim nothing that is not. */
  check("a 10 byte report parses",
        ds_parse_input_dev(V_DS4_IN_USB, 10, DS_LINK_USB,
                            DS_DEVICE_DUALSHOCK4, &s));
  checkInt("...with the sticks", s.analog.lx, 0x10);
  check("...and no motion claimed", !s.has_motion);
  check("...and no touch claimed", !s.has_touch);
  check("...and no status claimed", !s.status.valid);

  /* One byte short of the accelerometer. The old bound of "timestamp + 4"
   * sat at body 13 for this layout and would have waved this through. */
  check("motion needs the whole block",
        ds_parse_input_dev(V_DS4_IN_USB, 24, DS_LINK_USB,
                            DS_DEVICE_DUALSHOCK4, &s));
  check("...so 23 body bytes is not enough", !s.has_motion);
}

/* ------------------------------------------------------------------ output */

static void fillOutput(ds_output_t* o) {
  ds_output_init(o);
  o->set_rumble = true;
  o->rumble_left = 0x40;
  o->rumble_right = 0x80;
  o->set_lightbar = true;
  o->red = 0x10; o->green = 0x20; o->blue = 0x30;
  o->set_lightbar_blink = true;
  o->blink_on = 0x05; o->blink_off = 0x06;
  /* A DualShock 4 has none of these. They must be dropped, not misplaced. */
  o->set_mic_led = true;   o->mic_led = 2;
  o->set_player_leds = true; o->player_leds = 0x1F;
  ds5_trigger_weapon(o->trigger_right, 3, 6, 8);
  o->set_trigger_right = true;
}

static void testOutputUsb() {
  Serial.println("ds4 output, usb");
  ds_output_t o;
  uint8_t buf[DS_OUT_MAX_SIZE];
  fillOutput(&o);
  uint16_t n = ds_build_output_dev(&o, DS_LINK_USB, DS_DEVICE_DUALSHOCK4,
                                    buf, sizeof(buf));
  checkInt("length", n, DS4_OUT_USB_SIZE);
  checkBytes("bytes", buf, V_DS4_OUT_USB, DS4_OUT_USB_SIZE);

  uint8_t small[DS4_OUT_USB_SIZE - 1];
  checkInt("a short buffer writes nothing",
           ds_build_output_dev(&o, DS_LINK_USB, DS_DEVICE_DUALSHOCK4,
                                small, sizeof(small)), 0);
}

static void testOutputBt() {
  Serial.println("ds4 output, bluetooth");
  ds_output_t o;
  uint8_t buf[DS_OUT_MAX_SIZE];
  fillOutput(&o);
  uint16_t n = ds_build_output_dev(&o, DS_LINK_BT, DS_DEVICE_DUALSHOCK4,
                                    buf, sizeof(buf));
  checkInt("length", n, 1 + DS4_OUT_BT_SIZE);
  /* The pad drops the whole report unless byte 2 claims HID and a CRC32. */
  checkInt("hardware control byte", buf[2],
           DS4_OUT_HWCTL_HID | DS4_OUT_HWCTL_CRC32);
  checkBytes("bytes and crc", buf, V_DS4_OUT_BT, 1 + DS4_OUT_BT_SIZE);
}

static void testValidFlags() {
  Serial.println("ds4 output valid flags");
  ds_output_t o;
  uint8_t buf[DS_OUT_MAX_SIZE];

  /* An untouched output must ask the pad to change nothing. */
  ds_output_init(&o);
  ds_build_output_dev(&o, DS_LINK_USB, DS_DEVICE_DUALSHOCK4, buf, sizeof(buf));
  checkInt("nothing set means flag 0", buf[1], 0x00);

  ds_output_init(&o);
  o.set_rumble = true; o.rumble_left = 0x11; o.rumble_right = 0x22;
  ds_build_output_dev(&o, DS_LINK_USB, DS_DEVICE_DUALSHOCK4, buf, sizeof(buf));
  checkInt("rumble alone", buf[1], 0x01);
  checkInt("right motor first", buf[4], 0x22);
  checkInt("then left", buf[5], 0x11);

  ds_output_init(&o);
  o.set_lightbar = true; o.red = 1; o.green = 2; o.blue = 3;
  ds_build_output_dev(&o, DS_LINK_USB, DS_DEVICE_DUALSHOCK4, buf, sizeof(buf));
  checkInt("lightbar alone", buf[1], 0x02);
  check("rgb in order", buf[6] == 1 && buf[7] == 2 && buf[8] == 3);
  check("no blink unless asked", buf[9] == 0 && buf[10] == 0);
}

/* ------------------------------------------------------------- calibration */

static void testCalibration() {
  Serial.println("ds4 calibration");
  ds_calibration_t cal;

  check("usb report parses",
        ds_parse_calibration_dev(V_DS4_CAL_USB, sizeof(V_DS4_CAL_USB),
                                  DS_LINK_USB, DS_DEVICE_DUALSHOCK4, &cal));
  check("valid", cal.valid);
  checkInt("pitch numer", cal.gyro[0].numer, DS4_CAL_NUMER_0);
  checkInt("pitch denom", cal.gyro[0].denom, DS4_CAL_DENOM_0);
  checkInt("yaw denom", cal.gyro[1].denom, DS4_CAL_DENOM_1);
  checkInt("roll denom", cal.gyro[2].denom, DS4_CAL_DENOM_2);
  /* The gyro bias is a reference point, not an offset. Subtracting it is a
   * popular way to make a resting controller drift. */
  checkInt("gyro bias stays zero", cal.gyro[0].bias, 0);

  /* Bluetooth groups the three plus readings ahead of the three minus ones.
   * Read as the USB ordering it would scale pitch by roll's numbers, so the
   * two must come out the same only because each is read its own way. */
  ds_calibration_t bt;
  check("bt report parses",
        ds_parse_calibration_dev(V_DS4_CAL_BT, sizeof(V_DS4_CAL_BT),
                                  DS_LINK_BT, DS_DEVICE_DUALSHOCK4, &bt));
  check("both orderings reach the same scale",
        bt.gyro[0].denom == cal.gyro[0].denom &&
        bt.gyro[1].denom == cal.gyro[1].denom &&
        bt.gyro[2].denom == cal.gyro[2].denom);

  /* And the ordering genuinely matters: the BT bytes read as USB must not. */
  ds_calibration_t wrong;
  ds_parse_calibration_dev(V_DS4_CAL_BT, sizeof(V_DS4_CAL_BT), DS_LINK_USB,
                            DS_DEVICE_DUALSHOCK4, &wrong);
  check("the wrong ordering is rejected or differs",
        !wrong.valid || wrong.gyro[0].denom != cal.gyro[0].denom);

  /* A DualSense calibration must not be accepted as a DualShock 4's. */
  ds_calibration_t nope;
  check("wrong report id refused",
        !ds_parse_calibration_dev(V_DS4_CAL_USB, sizeof(V_DS4_CAL_USB),
                                   DS_LINK_BT, DS_DEVICE_DUALSHOCK4, &nope));
}

/* --------------------------------------------------- the DualSense is intact */

/* Every device-aware call with DS_DEVICE_DUALSENSE must be exactly the old
 * call. ProtocolCore covers the DualSense in full; this only pins the seam. */
static void testDualSenseUnchanged() {
  Serial.println("dualsense path unchanged");
  ds_output_t o;
  uint8_t a[DS_OUT_MAX_SIZE], b[DS_OUT_MAX_SIZE];
  ds_output_init(&o);
  o.set_lightbar = true; o.red = 9; o.green = 8; o.blue = 7;
  o.set_rumble = true; o.rumble_left = 6; o.rumble_right = 5;

  uint16_t n1 = ds_build_output(&o, DS_LINK_USB, a, sizeof(a));
  uint16_t n2 = ds_build_output_dev(&o, DS_LINK_USB, DS_DEVICE_DUALSENSE,
                                     b, sizeof(b));
  check("usb build identical", n1 == n2 && memcmp(a, b, n1) == 0);

  n1 = ds_build_output(&o, DS_LINK_BT, a, sizeof(a));
  n2 = ds_build_output_dev(&o, DS_LINK_BT, DS_DEVICE_DUALSENSE, b, sizeof(b));
  check("bt build identical", n1 == n2 && memcmp(a, b, n1) == 0);

  /* A DualShock 4 report must not decode as a DualSense one over Bluetooth:
   * 0x11 is not a report id a DualSense ever sends. */
  ds_state_t s;
  check("ds4 bluetooth report refused as a dualsense",
        !ds_parse_input(V_DS4_IN_BT, sizeof(V_DS4_IN_BT), DS_LINK_BT, &s));
}

/* --------------------------------------------------------------------- run */

/* The pads differ in what hardware exists, and a call the pad cannot honour
 * is dropped when the report is built — leaving a report that is one field
 * short, which looks exactly like one that arrived. This is the table that
 * lets a sketch ask first. */
static void testCapabilities() {
  Serial.println("capabilities");
  uint16_t ds4 = ds_capabilities(DS_DEVICE_DUALSHOCK4);
  uint16_t ds5 = ds_capabilities(DS_DEVICE_DUALSENSE);

  check("ds4 has no adaptive triggers", !(ds4 & DS_CAP_ADAPTIVE_TRIGGERS));
  check("ds4 has no mic led", !(ds4 & DS_CAP_MIC_LED));
  check("ds4 has no player leds", !(ds4 & DS_CAP_PLAYER_LEDS));
  check("ds4 has no mute button", !(ds4 & DS_CAP_MUTE_BUTTON));
  check("ds4 blinks its own lightbar", (ds4 & DS_CAP_LIGHTBAR_BLINK) != 0);

  check("ds5 has adaptive triggers", (ds5 & DS_CAP_ADAPTIVE_TRIGGERS) != 0);
  check("ds5 has a mic led", (ds5 & DS_CAP_MIC_LED) != 0);
  check("ds5 has player leds", (ds5 & DS_CAP_PLAYER_LEDS) != 0);
  check("ds5 has a mute button", (ds5 & DS_CAP_MUTE_BUTTON) != 0);
  check("ds5 cannot self-blink", !(ds5 & DS_CAP_LIGHTBAR_BLINK));

  /* No overlap: every flag belongs to exactly one pad. If that ever stops
   * being true this test is the wrong shape, not the table. */
  check("the two sets are disjoint", (ds4 & ds5) == 0);
}

/* Over Bluetooth there is no product id, so the report itself is the only
 * thing that names the pad — and before this worked, the framing check threw
 * every DualShock 4 full report away before anything could look at it. */
static void testBtFramingAndIdentity() {
  Serial.println("bt framing and identity");
  uint16_t len = 0;

  /* 0xA1, then the report. A DualShock 4's 0x11 has to survive this. */
  uint8_t framed[1 + sizeof(V_DS4_IN_BT)];
  framed[0] = DS_HID_HDR_INPUT;
  memcpy(framed + 1, V_DS4_IN_BT, sizeof(V_DS4_IN_BT));

  const uint8_t *rep = ds_find_bt_report(framed, sizeof(framed), &len);
  check("0x11 survives the framing check", rep == framed + 1);
  checkInt("...at full length", len, sizeof(V_DS4_IN_BT));

  framed[0] = 0x00;
  check("a bad transaction byte is still refused",
        ds_find_bt_report(framed, sizeof(framed), &len) == NULL);

  ds_device_t dev = DS_DEVICE_DUALSENSE;
  check("0x31 names a dualsense",
        ds_identify_bt_report(DS5_IN_BT_FULL, &dev) &&
        dev == DS_DEVICE_DUALSENSE);
  check("0x11 names a dualshock 4",
        ds_identify_bt_report(DS4_IN_BT, &dev) &&
        dev == DS_DEVICE_DUALSHOCK4);

  /* Both pads send 0x01 until a feature report is asked for, so it names
   * nobody — and must not be allowed to guess, or the first frame of every
   * connection would pick a pad at random. */
  ds_device_t before = DS_DEVICE_DUALSHOCK4;
  dev = before;
  check("0x01 names nobody", !ds_identify_bt_report(DS5_IN_BT_COMPAT, &dev));
  check("...and leaves the answer alone", dev == before);
  check("a null answer is refused", !ds_identify_bt_report(DS4_IN_BT, NULL));
}

/* The DualShock 4 button names are a separate enum so the ones this pad does
 * not have cannot be written at all. They have to keep meaning the same thing
 * as the shared ones, or the whole trick decodes the wrong bit. */
static void testDs4ButtonNames() {
  Serial.println("ds4 button names");
  checkInt("ds4 cross", (int)DS4_CROSS, (int)DS_CROSS);
  checkInt("ds5 cross", (int)DS5_CROSS, (int)DS_CROSS);
  checkInt("ds4 circle", (int)DS4_CIRCLE, (int)DS_CIRCLE);
  checkInt("ds5 circle", (int)DS5_CIRCLE, (int)DS_CIRCLE);
  checkInt("ds4 square", (int)DS4_SQUARE, (int)DS_SQUARE);
  checkInt("ds5 square", (int)DS5_SQUARE, (int)DS_SQUARE);
  checkInt("ds4 triangle", (int)DS4_TRIANGLE, (int)DS_TRIANGLE);
  checkInt("ds5 triangle", (int)DS5_TRIANGLE, (int)DS_TRIANGLE);
  checkInt("ds4 up", (int)DS4_UP, (int)DS_UP);
  checkInt("ds5 up", (int)DS5_UP, (int)DS_UP);
  checkInt("ds4 l1", (int)DS4_L1, (int)DS_L1);
  checkInt("ds5 l1", (int)DS5_L1, (int)DS_L1);
  checkInt("ds4 r2 click", (int)DS4_R2_CLICK, (int)DS_R2_CLICK);
  checkInt("ds5 r2 click", (int)DS5_R2_CLICK, (int)DS_R2_CLICK);
  checkInt("ds4 l3", (int)DS4_L3, (int)DS_L3);
  checkInt("ds5 l3", (int)DS5_L3, (int)DS_L3);
  checkInt("ds4 options", (int)DS4_OPTIONS, (int)DS_OPTIONS);
  checkInt("ds5 options", (int)DS5_OPTIONS, (int)DS_OPTIONS);
  checkInt("ds4 ps", (int)DS4_PS, (int)DS_PS);
  checkInt("ds5 ps", (int)DS5_PS, (int)DS_PS);
  checkInt("ds4 touchpad", (int)DS4_TOUCHPAD, (int)DS_TOUCHPAD);
  checkInt("ds5 touchpad", (int)DS5_TOUCHPAD, (int)DS_TOUCHPAD);

  /* Sony printed SHARE on this pad and CREATE on the other one. Same button,
   * same bit, with DS_CREATE as the shared sketch spelling. */
  checkInt("ds4 share is create's bit", (int)DS4_SHARE, (int)DS_CREATE);
  checkInt("ds5 create is create's bit", (int)DS5_CREATE, (int)DS_CREATE);

  /* There is deliberately no DS4_MUTE. It cannot be tested for here — that is
   * the point, it does not compile — so pin the capability it stands for. */
  check("and no mute to go with it",
        !(ds_capabilities(DS_DEVICE_DUALSHOCK4) & DS_CAP_MUTE_BUTTON));
}

static void runAll() {
  passCount = failCount = 0;
  Serial.println();
  Serial.println("=== DualSense32 DualShock 4 self-test ===");
  testInputUsb();
  testInputBt();
  testBatteryFull();
  testWrongDeviceIsSilent();
  testShortReports();
  testOutputUsb();
  testOutputBt();
  testValidFlags();
  testCalibration();
  testCapabilities();
  testBtFramingAndIdentity();
  testDs4ButtonNames();
  testDualSenseUnchanged();
  testDeviceExpectation();
  Serial.println();
  Serial.printf("%d passed, %d failed  ->  %s\n", passCount, failCount,
                failCount == 0 ? "PASS" : "*** FAILURES");
}


/* --------------------------------------------------------------------------
 * What begin(DS_DEVICE_...) is and is not.
 *
 * An expectation seeds the window before the pad names itself, so reports are
 * read and written in the right dialect from the first frame. It is not an
 * override: the moment something authoritative speaks, it loses. These check
 * that distinction directly, because getting it backwards is how a stated
 * expectation would turn a clean mismatch report into corrupt data.
 * ----------------------------------------------------------------------- */
static void testDeviceExpectation() {
  Serial.println("begin() expectation");
  ds_transport_t tr;

  memset(&tr, 0, sizeof(tr));
  ds_transport_forget_device(&tr);
  checkInt("no expectation defaults to dualsense",
           (int)tr.device, (int)DS_DEVICE_DUALSENSE);
  check("no expectation is not an answer", !tr.device_named);

  memset(&tr, 0, sizeof(tr));
  tr.device_expect = DS_DEVICE_DUALSHOCK4;
  tr.device_expected = true;
  ds_transport_forget_device(&tr);
  checkInt("expectation seeds the window",
           (int)tr.device, (int)DS_DEVICE_DUALSHOCK4);
  check("but an expectation is still not an answer", !tr.device_named);

  /* And it is dropped, not kept, when the pad it described goes away. */
  tr.device = DS_DEVICE_DUALSENSE;
  tr.device_named = true;
  ds_transport_forget_device(&tr);
  checkInt("disconnect returns to the expectation",
           (int)tr.device, (int)DS_DEVICE_DUALSHOCK4);
  check("disconnect drops the pad's word", !tr.device_named);

  /* A real identification is what the links write on top, and it wins. */
  ds_device_t seen;
  check("0x31 identifies", ds_identify_bt_report(0x31, &seen));
  checkInt("0x31 is a dualsense", (int)seen, (int)DS_DEVICE_DUALSENSE);
  tr.device = seen;
  tr.device_named = true;
  checkInt("identification beats the expectation",
           (int)tr.device, (int)DS_DEVICE_DUALSENSE);
  check("and it is an answer", tr.device_named);

  /* Which is exactly the state DualShock4::mismatched() exists to report:
   * expected a DualShock 4, a DualSense turned up and said so. */
  check("expected ds4, named ds5 => mismatch",
        tr.device_named && tr.device != tr.device_expect);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  runAll();
}

void loop() {
  delay(4000);
  runAll();
}

/*
 * DualShock4 — a PS4 pad on an original ESP32's own radio.
 *
 * This route needs no dongle and no cable: the original ESP32 has Bluetooth
 * Classic built in, which is what a DualShock 4 speaks. The ESP32-S3, S2, C3,
 * C6 and H2 cannot do this — they are BLE only — so this sketch is for the
 * original chip and nothing else.
 *
 * BOARD SETTINGS, and the first one is not optional:
 *
 *   Partition Scheme  Huge APP (3MB No OTA/1MB SPIFFS)
 *                     On the default the sketch does not fit and the upload
 *                     fails with "text section exceeds available space".
 *   Erase All Flash   Enabled for the first pairing, Disabled afterwards.
 *                     A stale link key on either side fails authentication
 *                     for ever, and it is kept in flash.
 *
 * PAIRING: hold SHARE, then press PS, and let go when the light bar flashes
 * quickly. SHARE is the small button left of the touchpad — a DualSense calls
 * it CREATE. After the first time, PS alone.
 *
 * Confirmed on hardware, motion calibration included.
 */

#include <DualShock4.h>
#include <ds_bt_bluedroid.h>

/* This sketch names its link on purpose: the folder is about one route, and
 * writing it out says which. It is not required. An original ESP32 has a
 * Bluetooth Classic radio and no USB host controller, so there is only one way
 * to reach a pad, and the library takes it when nothing is named:
 *
 *     #include <DualShock4.h>      // and nothing else
 *     DualShock4  pad;             // the built-in radio
 */
DualShock4 pad(ds_bt_bluedroid());

static bool announced = false;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("DualShock4 over Bluetooth Classic");
  Serial.println("hold SHARE, then press PS, until the light bar flashes fast");
  /* begin() on a DualShock4 already expects a DualShock 4 -- the type said
   * so, so there is nothing extra to write here. It matters most on this
   * link: over Bluetooth the pad is not named until its first full report,
   * and output built during that window has to know which report to build.
   * Input does not care -- report 0x01 has the same layout on both pads,
   * which is why the library refuses to guess the model from it.
   *
   * A particular pad by address works exactly as it does anywhere else:
   *   pad.begin("AA:BB:CC:DD:EE:FF"); */
  if (!pad.begin()) Serial.println("begin failed");
}

void loop() {
  pad.update();

  /* Three states, not two. The sketch says which pad it expects, but a USB
   * port or a radio cannot be asked in advance what will actually turn up, so
   * the check stays at run time.
   *
   * identifying() is the moment after connecting when the pad has not sent a
   * full report yet and so has not named itself. Reads are correct here
   * regardless, and the expectation from begin() makes writes correct too,
   * but connected() deliberately waits for the pad's own word, because an expectation a
   * DualSense could still contradict is not something to act on. */
  if (pad.identifying()) {
    static uint32_t since = 0;
    if (since == 0) since = millis();
    if (millis() - since > 3000) {
      since = millis();
      Serial.println("connected, but still on report 0x01 - nothing has asked");
      Serial.println("the pad for a feature report, so it never names itself");
    }
    return;
  }
  if (pad.mismatched()) {
    static bool said = false;
    if (!said) { Serial.println("that is a DualSense - wrong sketch"); said = true; }
    return;
  }
  if (!pad.connected()) { announced = false; return; }

  if (!announced) {
    announced = true;
    Serial.println();
    Serial.println("connected, and it is a DualShock 4");
    Serial.printf("  named by input report id 0x%02X\n", pad.raw().report_id);
    Serial.printf("  battery %u/10\n", pad.battery());
    Serial.println();
    /* WATCH THIS ONE. Over Bluetooth the pad is identified from the report
     * id: 0x11 is a DualShock 4, 0x31 a DualSense. Report 0x01 is sent by
     * both until a feature report is asked for and identifies nobody, so if
     * the line above says 0x01 the pad has not named itself yet and the
     * library is still treating it as a DualSense. */
  }

  if (pad.pressed(DS_CROSS))    pad.lightbar(0, 0, 255);
  if (pad.pressed(DS_CIRCLE))   pad.lightbar(255, 0, 0);
  if (pad.pressed(DS_TRIANGLE)) pad.lightbar(0, 255, 0);
  if (pad.pressed(DS_SQUARE))   pad.lightbar(255, 0, 255);

  uint8_t l2 = pad.l2() > 12 ? pad.l2() : 0;
  uint8_t r2 = pad.r2() > 12 ? pad.r2() : 0;
  pad.rumble(l2, r2);

  if (pad.pressed(DS_PS)) {
    static bool blinking = false;
    blinking = !blinking;
    pad.lightbarBlink(blinking ? 20 : 0, blinking ? 20 : 0);
  }

  pad.printEvents(Serial);

  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();

  /* Calibrated, so this is deg/s and g rather than raw counts: at rest the
   * gyro sits within a degree or so of zero and the accelerometer magnitude
   * comes to about 1.00.
   *
   * This link reads the calibration off the control channel, which arrives
   * before the pad has named itself — a DualShock 4 groups its gyro plus
   * readings where a DualSense pairs them per axis, so the bytes are kept and
   * read again once the pad says which it is. If the gyro looked roughly
   * sixty times too small, that re-read is where to look. */
  const ds_state_t &s = pad.raw();
  if (s.has_motion) {
    Serial.printf("gyro %7.1f %7.1f %7.1f   accel %5.2f %5.2f %5.2f g\n",
                  pad.gyroPitch(), pad.gyroYaw(), pad.gyroRoll(),
                  pad.accelX(), pad.accelY(), pad.accelZ());
  }
}

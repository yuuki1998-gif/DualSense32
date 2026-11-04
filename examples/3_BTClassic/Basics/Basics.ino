#include <ds_bt_bluedroid.h>
#include <DualSense32.h>

/*
 * Basics -- a DualSense, in as little code as it takes.
 *
 * Hold Cross to make the light bar red and the motors run. That is the whole
 * sketch: everything else in this folder is the same three calls with more
 * printed out.
 *
 *   pad.begin()     once
 *   pad.update()    at the top of loop(), and nothing works without it
 *   ...ask it things, and tell it things
 *
 * Hardware: an original ESP32, using its own Bluetooth Classic radio.
 *
 * Original ESP32 only: the S3, C3, C6 and H2 have no BR/EDR radio.
 * First run: hold Create, then PS, until the light bar flashes.
 */

#if !DS_HAS_BT_BLUEDROID

void setup() {
  Serial.begin(115200);
  delay(400);
}
void loop() {
  Serial.println("This sketch needs Bluetooth Classic: use an original ESP32.");
  delay(4000);
}

#else

/*
 * Which controller. Nothing to fill in: handed no address, the link runs an
 * inquiry and takes whatever answers as a gamepad, so a pad held in pairing
 * mode is found by itself. An already-paired one pressing PS is let in too,
 * because that call arrives here rather than going out.
 *
 * Name one to skip the search, or when several are within range:
 *
 *   pad.begin("AA:BB:CC:DD:EE:FF");
 *
 * To find the address, plug the pad into an S3 and run
 * 2_USBDirect/FeatureReports, or connect it once over a dongle -- both print
 * it. A typo returns false rather than searching for ever.
 */

/* This sketch names its link on purpose: the folder is about one route, and
 * writing it out says which. It is not required. An original ESP32 has a
 * Bluetooth Classic radio and no USB host controller, so there is only one way
 * to reach a pad, and the library takes it when nothing is named:
 *
 *     #include <DualSense32.h>     // and nothing else
 *     DualSense32 pad;             // the built-in radio
 */
DualSense32 pad(ds_bt_bluedroid());

/* The link narrates itself -- who it is paging, who called, why a channel was
 * refused. Without this it says nothing at all, and a wrong address looks the
 * same as a dead sketch. */
static void onLog(void *user, const char *msg) {
  (void)user;
  Serial.println(msg);
}

void setup() {
  Serial.begin(115200);
  ds_bt_on_log(pad.transport(), onLog, NULL);
  if (!pad.begin()) Serial.println("begin failed");
}

void loop() {
  pad.update();
  if (!pad.connected()) return;

  if (pad.pressed(DS_CROSS))  Serial.println("cross down");
  if (pad.released(DS_CROSS)) Serial.println("cross up");

  /* Setting is enough -- update() sends it. */
  if (pad.down(DS_CROSS)) {
    pad.lightbar(255, 0, 0);
    pad.rumble(120, 120);
  } else {
    pad.lightbar(0, 160, 255);
    pad.rumble(0, 0);
  }

  /*
   * Sticks arrive centred on zero, with a deadzone already applied. They
   * change on every report, a couple of hundred times a second, so this
   * prints a few times a second instead.
   *
   * Note the shape: the timer guards a block rather than returning out of
   * loop(). An early return here would slow down everything written below
   * it as well, which is a confusing thing to inherit.
   */
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 200) {
    lastPrint = millis();
    if (pad.leftX() || pad.leftY()) {
      Serial.printf("left stick %d,%d\n", pad.leftX(), pad.leftY());
    }
  }
}
#endif /* DS_HAS_BT_BLUEDROID */

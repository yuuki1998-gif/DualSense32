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
 * Hardware: an ESP32-S3 or S2 with a Bluetooth dongle in the USB host port.
 *
 * First run: hold Create, then PS, until the light bar flashes.
 * After that: press PS. The pairing is kept in flash by the link itself.
 */

DualSense32 pad;

void setup() {
  Serial.begin(115200);
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

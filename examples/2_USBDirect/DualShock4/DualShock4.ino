/*
 * DualShock4 — a PS4 pad on the wire.
 *
 * Written for a DualShock 4 and nothing else, so it says so in the type:
 *
 *     DualShock4 pad(ds_link_usb_hid());
 *
 * Shared buttons are DS_* on any pad. The names this pad prints on itself are
 * DS4_* — DS4_SHARE, where a DualSense says CREATE — and the ones it does not
 * have are not merely false, they do not exist:
 *
 *     pad.down(DS4_MUTE);   error: 'DS4_MUTE' was not declared in this scope
 *     pad.micLed(2);        error: use of deleted function
 *     pad.triggerRight(b);  error: use of deleted function
 *
 * A DualShock 4 has no adaptive triggers, no microphone and no player LEDs.
 * What it has instead is a light bar it blinks by itself.
 *
 * If you want one sketch that takes whichever pad is plugged in, use
 * DualSense32 and ask pad.supports(...) — EitherPad in this folder is written
 * that way.
 *
 * Wiring: the pad goes in the S3's *native* USB port, over a USB-C to USB-A
 * adapter and the pad's own cable. Leave the UART port in the PC for power
 * and this log.
 *
 * Leave Tools > USB CDC On Boot at Disabled. The native port is being a USB
 * *host* for the pad, so it cannot also be a CDC serial device; with CDC
 * enabled, Serial goes to that port and nothing reaches the monitor.
 */

#include <DualShock4.h>
#include <ds_link_usb_hid.h>

DualShock4 pad(ds_link_usb_hid());

static bool announced = false;

static void announce() {
  Serial.println();
  Serial.println("DualShock 4 attached");
  Serial.println("  touchpad 1920 x 942, no adaptive triggers, no mic");

  char mac[18];
  if (pad.address(mac)) Serial.printf("  bluetooth address %s\n", mac);
  Serial.printf("  battery %u/10%s\n", pad.battery(),
                pad.charging() ? ", charging" : "");
  Serial.println();
  Serial.println("Try: face buttons change the colour, the triggers rumble,");
  Serial.println("     PS blinks the light bar, touch to see coordinates.");
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("plug a DualShock 4 into the native USB port...");
  if (!pad.begin()) Serial.println("begin failed");
}

void loop() {
  pad.update();

  /* A USB port cannot be asked in advance what will be plugged into it, so
   * this one check stays at run time. Without it a DualSense would have its
   * reports decoded as a DualShock 4's: no crash, no error, just a sketch
   * that never sees a button. */
  if (pad.mismatched()) {
    static bool said = false;
    if (!said) { Serial.println("that is a DualSense - wrong sketch"); said = true; }
    return;
  }
  if (!pad.connected()) { announced = false; return; }
  /* Not on the first connected frame. On this link the product id names the
   * pad the moment it attaches, which is before any input report has been
   * parsed -- so the snapshot everything below reads from is still zeroed, and
   * announcing there prints a battery of 0. A report id means one has landed. */
  if (!announced && pad.raw().report_id != 0) { announce(); announced = true; }

  if (pad.pressed(DS_CROSS))    pad.lightbar(0, 0, 255);
  if (pad.pressed(DS_CIRCLE))   pad.lightbar(255, 0, 0);
  if (pad.pressed(DS_TRIANGLE)) pad.lightbar(0, 255, 0);
  if (pad.pressed(DS_SQUARE))   pad.lightbar(255, 0, 255);

  /* Each trigger drives the motor on its own side; rumble() takes left
   * first. A small deadzone, because a worn trigger does not always come all
   * the way back and a motor running on its own drowns out everything else. */
  uint8_t l2 = pad.l2() > 12 ? pad.l2() : 0;
  uint8_t r2 = pad.r2() > 12 ? pad.r2() : 0;
  pad.rumble(l2, r2);

  /* The pad blinks by itself, in units of 10 ms, and keeps doing it while the
   * sketch is busy elsewhere. A DualSense cannot do this. */
  if (pad.pressed(DS_PS)) {
    static bool blinking = false;
    blinking = !blinking;
    pad.lightbarBlink(blinking ? 20 : 0, blinking ? 20 : 0);
    Serial.printf("light bar %s\n", blinking ? "blinking" : "solid");
  }

  /* --- report what the pad is doing, a few times a second --------------- */

  static uint32_t last = 0;
  if (millis() - last < 250) return;
  last = millis();

  const ds_state_t &s = pad.raw();

  Serial.printf("L %4d,%4d  R %4d,%4d  L2 %3u R2 %3u  batt %u/10",
                pad.leftX(), pad.leftY(), pad.rightX(), pad.rightY(),
                pad.l2(), pad.r2(), pad.battery());

  if (s.has_motion) {
    Serial.printf("  gyro %6.1f %6.1f %6.1f  accel %5.2f %5.2f %5.2f",
                  pad.gyroPitch(), pad.gyroYaw(), pad.gyroRoll(),
                  pad.accelX(), pad.accelY(), pad.accelZ());
  }
  if (s.has_touch && s.touch[0].active) {
    /* 1920 across on either pad, and this one is 942 tall where a DualSense
     * is 1080 — scale against the right one or the bottom is unreachable. */
    Serial.printf("  touch %u,%u of %u,%u", s.touch[0].x, s.touch[0].y,
                  (unsigned)DS4_TOUCH_WIDTH, (unsigned)DS4_TOUCH_HEIGHT);
  }
  Serial.println();
}

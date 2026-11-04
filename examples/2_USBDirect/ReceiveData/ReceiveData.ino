#include <ds_link_usb_hid.h>
#include <DualSense32.h>

/*
 * ReceiveData -- how to read every part of a DualSense.
 *
 * Written to be copied from. Each section below is the shortest correct way
 * to get at one kind of input, with the names you would type spelled out.
 *
 * Hardware: an ESP32-S3 or S2 with the pad on a USB-A to USB-C cable.
 *
 * No radio and no pairing: plug it in and it is there.
 */

DualSense32 pad(ds_link_usb_hid());

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nReceiveData - press, tilt and touch things");

  /*
   * No address takes whichever bonded pad connects. To hold out for one
   * particular controller, name it, written as you would read it off a
   * screen -- a typo returns false rather than waiting for ever:
   *
   *   pad.begin("AA:BB:CC:DD:EE:FF");
   */
  if (!pad.begin()) Serial.println("begin failed");

  /* A resting stick wanders a few counts either side of centre. Eight is a
   * quiet default; pad.deadzone(0) shows the raw wander instead. */
  pad.deadzone(8);
}

/* ========================================================================== */
/*  buttons                                                                   */
/* ========================================================================== */

/*
 * All nineteen, and these are the names to type:
 *
 *   DS_UP        DS_RIGHT     DS_DOWN      DS_LEFT
 *   DS_SQUARE    DS_CROSS     DS_CIRCLE    DS_TRIANGLE
 *   DS_L1        DS_R1        DS_L2_CLICK  DS_R2_CLICK
 *   DS_CREATE    DS_OPTIONS   DS_L3        DS_R3
 *   DS_PS        DS_TOUCHPAD  DS5_MUTE
 *
 * L2_CLICK and R2_CLICK are the click at the bottom of the trigger travel.
 * The pressure on the way down is an axis: pad.l2() and pad.r2().
 *
 * Three ways to ask, and picking the wrong one is the usual first bug:
 *
 *   pad.pressed(b)    true on the one frame it goes down
 *   pad.released(b)   true on the one frame it comes back up
 *   pad.down(b)       true every frame it is held -- hundreds a second
 */
static void buttons() {
  if (pad.pressed(DS_CROSS))    Serial.println("cross");
  if (pad.pressed(DS_CIRCLE))   Serial.println("circle");
  if (pad.pressed(DS_SQUARE))   Serial.println("square");
  if (pad.pressed(DS_TRIANGLE)) Serial.println("triangle");

  if (pad.released(DS_CROSS))   Serial.println("cross let go");

  /* Or walk the lot, when you want them all. buttonName() is there for
   * exactly this. */
  for (int i = 0; i < DS5_BUTTON_COUNT; i++) {
    DS5Button b = (DS5Button)i;
    if (b == DS_CROSS || b == DS_CIRCLE || b == DS_SQUARE ||
        b == DS_TRIANGLE) {
      continue;   /* already said above */
    }
    if (pad.pressed(b)) Serial.printf("%s\n", DualSense32::buttonName(b));
  }
}

/* ========================================================================== */
/*  sticks and triggers                                                       */
/* ========================================================================== */

/*
 * Sticks come back centred on zero, -128 to 127, with the deadzone applied.
 * Up is negative on both Y axes, the way screen coordinates run.
 *
 *   pad.leftX()   pad.leftY()   pad.rightX()   pad.rightY()
 *
 * Triggers are pressure, 0 to 255, and are not centred on anything:
 *
 *   pad.l2()      pad.r2()
 */
static void sticks() {
  if (!pad.leftX() && !pad.leftY() && !pad.rightX() && !pad.rightY() &&
      !pad.l2() && !pad.r2()) {
    return;
  }
  Serial.printf("stick  L %4d,%4d   R %4d,%4d   L2 %3u  R2 %3u\n",
                pad.leftX(), pad.leftY(), pad.rightX(), pad.rightY(),
                pad.l2(), pad.r2());
}

/* ========================================================================== */
/*  touchpad, motion, battery                                                 */
/* ========================================================================== */

/*
 * Anything this class does not wrap is in pad.raw(), which is the struct the
 * library decoded straight from the report.
 *
 *   raw().touch[0..1]   .active .id .x (0-1919) .y (0-1079)
 *   gyroPitch/Yaw/Roll()   deg/s, already divided
 *   accelX/Y/Z()           g, already divided
 *   raw().motion           the same thing as LSB counts
 *   rawMotion()            counts before the factory calibration
 *   raw().status           .jack .mic_muted, plus battery() and charging()
 *   raw().has_touch / .has_motion
 *
 * A finger keeps its id for as long as it stays down, so a drag is one
 * contact and a re-tap is a new one.
 */
static void touch() {
  const ds_state_t& s = pad.raw();
  static bool wasDown[2];
  static uint32_t lastDrag = 0;

  /*
   * Three cases, and they want different rates. Landing and lifting are
   * checked every frame, because a tap is easily shorter than any throttle
   * worth having. The position while a finger stays down arrives a couple of
   * hundred times a second, so that one is slowed.
   */
  bool drag = millis() - lastDrag >= 200;

  for (int i = 0; i < 2; i++) {
    bool isDown = s.has_touch && s.touch[i].active;
    if (isDown && !wasDown[i]) {
      Serial.printf("touch  finger %d down  id %-3u  %4u,%4u\n", i,
                    s.touch[i].id, s.touch[i].x, s.touch[i].y);
    } else if (!isDown && wasDown[i]) {
      Serial.printf("touch  finger %d up\n", i);
    } else if (isDown && drag) {
      Serial.printf("touch  finger %d      id %-3u  %4u,%4u\n", i,
                    s.touch[i].id, s.touch[i].x, s.touch[i].y);
    }
    wasDown[i] = isDown;
  }
  if (drag) lastDrag = millis();
}

/* Held on a button and slowed down, because a controller lying on a desk is
 * never quite still and this arrives with every report. Calibrated gyro
 * counts are 1024 per degree per second. */
static void motion() {
  const ds_state_t& s = pad.raw();
  if (pad.down(DS_L1) && s.has_motion) {
    /* The accessors divide by the right constant for each sensor; the raw
     * counts are printed beside them so the scale is visible rather than
     * taken on trust. Held still, one accelerometer axis reads about 1 g. */
    Serial.printf("motion  %6.1f %6.1f %6.1f deg/s | %5.2f %5.2f %5.2f g"
                  "   (lsb %ld %ld %ld / %d %d %d)\n",
                  pad.gyroPitch(), pad.gyroYaw(), pad.gyroRoll(),
                  pad.accelX(), pad.accelY(), pad.accelZ(),
                  (long)s.motion.gyro[0], (long)s.motion.gyro[1],
                  (long)s.motion.gyro[2], s.motion.accel[0], s.motion.accel[1],
                  s.motion.accel[2]);
  }
}

static void status() {
  const ds_status_t& st = pad.raw().status;
  static uint8_t was = 255;

  if (!st.valid || st.battery == was) return;
  was = st.battery;
  Serial.printf("battery %u/10%s  jack %s  mic %s\n", pad.battery(),
                pad.charging() ? " charging" : "", st.jack ? "in" : "out",
                st.mic_muted ? "muted" : "live");
}

/* ========================================================================== */

void loop() {
  /* Pumps the link and takes the snapshot everything below reads from.
   * Nothing changes without it. */
  pad.update();

  static bool was = false;
  if (pad.connected() != was) {
    was = pad.connected();
    Serial.println(was ? "link: connected" : "link: gone");
  }
  if (!pad.connected()) return;

  /* The light bar says the library has the pad, not just the radio.
   * Setting it is enough; update() sends it. */
  pad.lightbar(0, 160, 255);

  buttons();          /* every frame: an edge lasts one frame */
  touch();            /* likewise -- a tap is shorter than any throttle */
  status();

  /*
   * Sticks and motion change on every report, so these run a few times a
   * second. The timer guards a block rather than returning out of loop():
   * an early return would slow down anything written below it too.
   */
  static uint32_t lastSlow = 0;
  if (millis() - lastSlow >= 200) {
    lastSlow = millis();
    sticks();
    motion();
  }

  /*
   * All of the above, and the throttling, in one call:
   *
   *   pad.printEvents(Serial);      one line per change, nothing otherwise
   *   pad.printState(Serial);       everything at once, for a dump key
   */
}

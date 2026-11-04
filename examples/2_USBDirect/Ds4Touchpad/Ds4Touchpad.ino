/*
 * Ds4Touchpad — using the touchpad, not just printing it.
 *
 * The touchpad reports two fingers at once, each with a coordinate and an id:
 *
 *     const ds_state_t &s = pad.raw();
 *     s.has_touch          // false on the short compatibility report
 *     s.touch[0].active    // is a finger down
 *     s.touch[0].id        // bumped on each new contact, so a drag keeps its id
 *     s.touch[0].x         // 0-1919
 *     s.touch[0].y         // 0-1079 on a DualSense, 0-942 on a DualShock 4
 *
 * The id is what makes a drag possible. Two frames with a finger down are not
 * necessarily the same finger -- one may have lifted and another landed between
 * them -- and the coordinate alone cannot tell you. The id can: while it holds,
 * it is one continuous contact, and the moment it changes the drag is over.
 * Tracking position without it produces a jump every time somebody lifts and
 * re-lands, which looks like a glitch and is not.
 *
 * The gesture is decided when the finger lifts, from where it started, where it
 * ended and how long it took. Deciding mid-drag needs two thresholds -- one to
 * start looking and one to classify -- and if the first is looser than the
 * second, everything between them falls into the small case. This sketch had
 * exactly that bug and reported a tap for every drag, never once a swipe. It
 * built cleanly the whole time; only the pad showed it.
 *
 * The vertical range differs between the pads, which matters as soon as you
 * turn a coordinate into a fraction -- and again when you pick a threshold,
 * because the pad is twice as wide as it is tall, so one distance in counts is
 * not one distance in feel. Both are scaled per axis below.
 *
 * Wiring and board settings: as in DualShock4 in this folder -- pad in the
 * native USB port, Tools > USB CDC On Boot at Disabled.
 */

#include <DualShock4.h>
#include <ds_link_usb_hid.h>

DualShock4 pad(ds_link_usb_hid());

static const uint16_t TOUCH_MAX_X = 1919;
static const uint16_t TOUCH_MAX_Y = 942;    /* a DualSense goes to 1079 */

/* A tap is small and quick. The slop is generous because a thumb rolls. */
static const uint16_t TAP_SLOP_X = 140;
static const uint16_t TAP_SLOP_Y = 90;
static const uint32_t TAP_MAX_MS = 350;

/* A swipe crosses a fifth of the pad, measured on the axis it travelled. */
static const uint16_t SWIPE_MIN_X = TOUCH_MAX_X / 5;   /* 383 */
static const uint16_t SWIPE_MIN_Y = TOUCH_MAX_Y / 5;   /* 188 */

static bool announced = false;

/* Per-slot contact state, keyed by id: a slot is only where the pad happened
 * to put a contact this frame. */
struct Contact {
  bool active;
  uint8_t id;
  uint16_t startX, startY;
  uint16_t lastX, lastY;
  uint32_t startMs;
};
static Contact contact[2];

static int absi(int v) { return v < 0 ? -v : v; }

static void classify(const Contact &c) {
  int dx = (int)c.lastX - (int)c.startX;
  int dy = (int)c.lastY - (int)c.startY;
  uint32_t dt = millis() - c.startMs;

  if (absi(dx) < TAP_SLOP_X && absi(dy) < TAP_SLOP_Y && dt < TAP_MAX_MS) {
    Serial.printf("tap          (%+5d,%+5d in %4lu ms)\n", dx, dy,
                  (unsigned long)dt);
    pad.lightbar(255, 255, 255);
    return;
  }

  /* Which axis travelled further, as a fraction of that axis rather than in
   * counts, because the two axes are not the same length. */
  bool horizontal = ((long)absi(dx) * TOUCH_MAX_Y) >
                    ((long)absi(dy) * TOUCH_MAX_X);

  if (horizontal && absi(dx) >= SWIPE_MIN_X) {
    Serial.printf("swipe %-5s  (%+5d,%+5d in %4lu ms)\n",
                  dx > 0 ? "right" : "left", dx, dy, (unsigned long)dt);
    pad.lightbar(dx > 0 ? 0 : 255, dx > 0 ? 255 : 0, 0);
    return;
  }
  if (!horizontal && absi(dy) >= SWIPE_MIN_Y) {
    Serial.printf("swipe %-5s  (%+5d,%+5d in %4lu ms)\n",
                  dy > 0 ? "down" : "up", dx, dy, (unsigned long)dt);
    pad.lightbar(0, 0, dy > 0 ? 255 : 80);
    return;
  }

  /* Too far to be a tap, not far enough to be a swipe, or simply too slow. */
  Serial.printf("press        (%+5d,%+5d in %4lu ms)\n", dx, dy,
                (unsigned long)dt);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("Ds4Touchpad - plug in a DualShock 4");
  if (!pad.begin()) Serial.println("begin failed");
}

void loop() {
  pad.update();

  if (pad.mismatched()) {
    static bool said = false;
    if (!said) { Serial.println("that is a DualSense - wrong sketch"); said = true; }
    return;
  }
  if (!pad.connected()) { announced = false; return; }

  const ds_state_t &s = pad.raw();

  /* Nothing has been parsed yet on the first connected frame: over USB the
   * product id names the pad at attach, ahead of any input report. */
  if (s.report_id == 0) return;

  if (!announced) {
    announced = true;
    Serial.println();
    Serial.println("connected. Tap, drag or swipe; two fingers work.");
    Serial.printf("pad reads %u x %u\n", TOUCH_MAX_X + 1, TOUCH_MAX_Y + 1);
    Serial.println();
  }

  /* Without this the pad is still on the short compatibility report, which
   * carries no touch data at all -- every finger would read as lifted. */
  if (!s.has_touch) return;

  for (int i = 0; i < 2; i++) {
    const ds_touch_point_t &t = s.touch[i];
    Contact &c = contact[i];

    if (!t.active) {
      if (c.active) {
        c.active = false;
        Serial.printf("finger %d up   ", i);
        classify(c);
      }
      continue;
    }

    /* A new contact, either because nothing was down or because the previous
     * one ended and another began between two frames. */
    if (!c.active || c.id != t.id) {
      c.active = true;
      c.id = t.id;
      c.startX = c.lastX = t.x;
      c.startY = c.lastY = t.y;
      c.startMs = millis();
      Serial.printf("finger %d down  id %-3u at %4u,%4u\n", i, t.id, t.x, t.y);
      continue;
    }

    c.lastX = t.x;
    c.lastY = t.y;
  }

  /* The first finger's position as a fraction of the pad, which is where the
   * different vertical range would bite if it were assumed. */
  static uint32_t last = 0;
  if (contact[0].active && millis() - last > 400) {
    last = millis();
    Serial.printf("   x %3d%%   y %3d%%\n",
                  (int)(contact[0].lastX * 100L / TOUCH_MAX_X),
                  (int)(contact[0].lastY * 100L / TOUCH_MAX_Y));
  }
}

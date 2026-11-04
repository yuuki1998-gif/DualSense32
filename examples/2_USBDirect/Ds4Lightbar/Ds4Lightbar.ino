/*
 * Ds4Lightbar — the one output a DualShock 4 has and a DualSense does not.
 *
 * Both pads have a light bar and both take a colour. Only a DualShock 4 can be
 * told to blink it:
 *
 *     pad.lightbarBlink(on, off);   // units of 10 ms, both zero is solid
 *
 * The difference is who keeps time. A DualSense blinks only while your loop()
 * keeps setting and clearing the colour, so the rhythm stops the moment the
 * sketch is busy doing something else. A DualShock 4 is given the two numbers
 * once and blinks on its own -- through a long computation, through a blocking
 * read, through anything short of losing the link. That makes it usable as a
 * status light rather than an effect, which is what this sketch does with it.
 *
 * The colour is set separately and stays under your control while it blinks:
 * changing it mid-blink works, and the new colour simply takes over the same
 * rhythm. Colour says what kind of state; the rate says how urgent.
 *
 * No state here stops blinking, and none passes an off of zero. Both are
 * deliberate. The pad takes a new colour or a new rate only at a blink
 * boundary, so an off of zero -- which removes the boundary -- leaves it
 * showing whatever it had and ignoring what comes next; lightbarBlink(0, 0)
 * simply puts the bar out. The lit end of the scale is therefore a long on
 * with an off of 1: ten milliseconds of dark cannot be seen, and twenty can.
 * See VERIFICATION.md, 2026-08-23.
 *
 * Wiring and board settings: as in DualShock4 in this folder -- pad in the
 * native USB port, Tools > USB CDC On Boot at Disabled.
 */

#include <DualShock4.h>
#include <ds_link_usb_hid.h>

DualShock4 pad(ds_link_usb_hid());

/* Blink rates, in units of 10 ms on and off. Slower reads as calmer, and a
 * long on with a short off reads as lit rather than as blinking. */
static const uint8_t REST_ON = 200, REST_OFF = 1;    /* lit: 2 s on, 10 ms off  */
static const uint8_t CALM_ON = 8,   CALM_OFF = 90;   /* a brief pulse, long gap */
static const uint8_t WARN_ON = 30,  WARN_OFF = 30;   /* an even, insistent beat */
static const uint8_t ALARM_ON = 6,  ALARM_OFF = 6;   /* fast enough to nag      */

enum State { IDLE, WORKING, LOW_BATTERY, FAULT };
static State state = IDLE;
static bool announced = false;

static void show(State s) {
  switch (s) {
    case IDLE:
      pad.lightbar(0, 160, 255);                 /* the library's own blue */
      pad.lightbarBlink(REST_ON, REST_OFF);      /* reads as lit */
      break;
    case WORKING:
      pad.lightbar(0, 255, 0);
      pad.lightbarBlink(CALM_ON, CALM_OFF);
      break;
    case LOW_BATTERY:
      pad.lightbar(255, 140, 0);
      pad.lightbarBlink(WARN_ON, WARN_OFF);
      break;
    case FAULT:
      pad.lightbar(255, 0, 0);
      pad.lightbarBlink(ALARM_ON, ALARM_OFF);
      break;
  }
}

static const char *name(State s) {
  switch (s) {
    case IDLE:        return "idle - blue, lit";
    case WORKING:     return "working - green, slow pulse";
    case LOW_BATTERY: return "low battery - amber, steady blink";
    case FAULT:       return "fault - red, fast blink";
  }
  return "";
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("Ds4Lightbar - plug in a DualShock 4");
  if (!pad.begin()) Serial.println("begin failed");
}

void loop() {
  pad.update();

  if (pad.mismatched()) {
    static bool said = false;
    if (!said) {
      Serial.println("that is a DualSense - it cannot blink on its own, so this");
      Serial.println("sketch has nothing to show. Use EitherPad instead.");
      said = true;
    }
    return;
  }
  if (!pad.connected()) { announced = false; return; }

  /* Over USB the product id names the pad the moment it attaches, ahead of any
   * input report, so the first connected frame still reads an empty snapshot. */
  if (pad.raw().report_id == 0) return;

  if (!announced) {
    announced = true;
    Serial.println();
    Serial.println("connected. CROSS cycles the state, PS holds the pad still");
    Serial.println("so you can watch the pad keep the rhythm without help.");
    Serial.println();
    show(state);
    Serial.println(name(state));
  }

  if (pad.pressed(DS_CROSS)) {
    state = (State)((state + 1) % 4);
    show(state);
    Serial.println(name(state));
  }

  /* The point of the whole thing. While this blocks, loop() is not running and
   * nothing is touching the light bar -- and the pad blinks anyway. Try the
   * same trick on a DualSense and the light bar simply freezes. */
  if (pad.pressed(DS_PS)) {
    Serial.println("blocking for three seconds - the blink should not stutter");
    delay(3000);
    Serial.println("back");
  }
}

/*
 * EitherPad — one binary, either controller, no #ifdef.
 *
 * The other examples here are written for a pad the author already knows. This
 * one is not. It asks what turned up and adapts, which is the case the library
 * is shaped around:
 *
 *     DualSense32 pad(ds_link_usb_hid());   // not DualShock4, not DualSense-only
 *
 * The question it asks is pad.supports(), never "which pad is this". Those are
 * different questions and only one of them keeps working. A sketch that tests
 * for a DualSense to decide whether it may set the mic LED breaks on the day a
 * controller this code has never heard of turns up with a mic; a sketch that
 * asks whether there is a mic does not.
 *
 *     if (pad.supports(DS_CAP_MIC_LED)) pad.micLed(1);
 *
 * Same idea for output that exists on both pads but is reached differently.
 * Showing "player two" is player LEDs on a DualSense and a blinking light bar
 * on a DualShock 4 -- the same intent, two mechanisms, chosen once below in
 * showPlayerNumber() rather than sprinkled through the sketch.
 *
 * Wiring: the pad goes in the S3's *native* USB port, over a USB-C to USB-A
 * adapter and the pad's own cable. Leave the UART port in the PC for power and
 * this log.
 *
 * Leave Tools > USB CDC On Boot at Disabled. The native port is being a USB
 * host for the pad, so it cannot also be a CDC serial device; with CDC enabled
 * Serial goes to that port and nothing reaches the monitor.
 *
 * Try it with both pads, one after the other. Nothing below changes.
 */

#include <DualSense32.h>
#include <ds_link_usb_hid.h>

DualSense32 pad(ds_link_usb_hid());

static bool announced = false;
static uint8_t player = 1;

/* ---------------------------------------------------------------------------
 * Say which player this is, using whatever this pad has for saying it.
 *
 * A DualSense has five lamps under the touchpad. A DualShock 4 has none, but
 * it can blink its light bar without being told to each time, so the number is
 * the blink rate there. Neither branch is a fallback for the other -- they are
 * two ways to do the same job, and the pad decides which one is available.
 * ------------------------------------------------------------------------- */
static void showPlayerNumber(uint8_t n) {
  if (pad.supports(DS_CAP_PLAYER_LEDS)) {
    static const uint16_t PATTERN[] = {0, 100, 1010, 10101, 11011, 11111};
    pad.playerLeds(PATTERN[n <= 5 ? n : 5]);
    return;
  }
  if (pad.supports(DS_CAP_LIGHTBAR_BLINK)) {
    /* Units of 10 ms. One blink for player one, faster for each after it. */
    uint8_t period = (uint8_t)(60 - (n * 8));
    pad.lightbarBlink(period, period);
    return;
  }
  /* Nothing to say it with. Not an error -- just a pad without either. */
}

static void describe() {
  uint16_t caps = pad.capabilities();

  Serial.println();
  Serial.print("connected: ");
  Serial.println(pad.isDualShock4() ? "DualShock 4" : "DualSense");
  Serial.printf("  named by the link: %s\n", pad.deviceNamed() ? "yes" : "not yet");
  Serial.printf("  battery %u/10%s\n", pad.battery(), pad.charging() ? ", charging" : "");

  Serial.println("  it has:");
  if (caps & DS_CAP_ADAPTIVE_TRIGGERS) Serial.println("    adaptive triggers");
  if (caps & DS_CAP_MIC_LED)           Serial.println("    a microphone lamp");
  if (caps & DS_CAP_PLAYER_LEDS)       Serial.println("    player LEDs");
  if (caps & DS_CAP_MUTE_BUTTON)       Serial.println("    a mute button");
  if (caps & DS_CAP_LIGHTBAR_BLINK)    Serial.println("    a light bar it blinks itself");

  Serial.println();
  Serial.println("  CROSS/CIRCLE/TRIANGLE/SQUARE set the colour");
  Serial.println("  L1 and R1 change the player number");
  Serial.println("  OPTIONS asks for something this pad may not have");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("EitherPad - plug in a DualSense or a DualShock 4");
  if (!pad.begin()) Serial.println("begin failed");
}

void loop() {
  pad.update();

  if (!pad.connected()) {
    announced = false;
    return;
  }

  /* Over Bluetooth the pad is not named for a moment after connecting. On this
   * link the product id names it immediately, but asking costs nothing and the
   * sketch then moves to another link unchanged. */
  if (!pad.deviceNamed()) return;

  /* And deviceNamed() is not the same as "a report has arrived". Over USB the
   * product id names the pad at attach, before anything has been parsed, so
   * the snapshot is still zeroed and the summary below would report a battery
   * of 0. Over Bluetooth the two coincide, because there the pad is named by a
   * report -- which is exactly why this is easy to miss. */
  if (pad.raw().report_id == 0) return;

  if (!announced) {
    announced = true;
    describe();
    pad.lightbar(0, 160, 255);
    showPlayerNumber(player);
  }

  if (pad.pressed(DS_CROSS))    pad.lightbar(0, 0, 255);
  if (pad.pressed(DS_CIRCLE))   pad.lightbar(255, 0, 0);
  if (pad.pressed(DS_TRIANGLE)) pad.lightbar(0, 255, 0);
  if (pad.pressed(DS_SQUARE))   pad.lightbar(255, 0, 255);

  if (pad.pressed(DS_R1) && player < 5) showPlayerNumber(++player);
  if (pad.pressed(DS_L1) && player > 1) showPlayerNumber(--player);

  /* Reaching for hardware without asking first. On a DualSense this pulses the
   * microphone lamp; on a DualShock 4 it changes nothing and returns false.
   * Either way the sketch is told, which is the point -- the call is refused
   * where it can be seen rather than dropped in silence. */
  if (pad.pressed(DS_OPTIONS)) {
    if (!pad.micLed(2)) {
      Serial.printf("micLed refused: no mic lamp on a %s\n",
                    pad.isDualShock4() ? "DualShock 4" : "DualSense");
      Serial.printf("  refusals so far: %u, last was %s\n",
                    pad.refusedCount(), pad.lastRefused());
      pad.clearRefused();
    }
  }

  /* Both triggers drive the motors on every pad. */
  pad.rumble(pad.l2(), pad.r2());
}

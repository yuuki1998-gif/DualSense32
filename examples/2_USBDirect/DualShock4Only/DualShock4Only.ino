/*
 * DualShock4Only — name the pad in the sketch, and let the compiler check.
 *
 * The other DualShock4 example uses DualSense32, which takes whichever pad is
 * plugged in and answers pad.supports(...) at run time. This one is written
 * for a PS4 pad and nothing else, so it says so in the type:
 *
 *     DualShock4 pad(ds_link_usb_hid());
 *
 * Buttons can use DSButton names such as DS_CROSS and DS_CREATE.
 * DS4Button aliases such as DS4_SHARE are still there when the printed
 * DualShock 4 names matter. There is no DS4_MUTE at all, so
 * reaching for the mute button is an undeclared identifier:
 *
 *     error: 'DS4_MUTE' was not declared in this scope
 *
 * and so is the hardware behind it, deleted so the error names the call:
 *
 *     error: use of deleted function 'bool DualShock4::triggerRight(...)'
 *     error: use of deleted function 'bool DualShock4::micLed(uint8_t)'
 *
 * Writing a DualSense button alias here fails too, even for one both pads
 * have. CROSS exists on either, but reaching for DS5_CROSS says the sketch
 * was written against the other controller.
 *
 * A DualShock 4 has no adaptive triggers, no microphone and no player LEDs.
 * Through DualSense32 those calls return false while the sketch runs, which
 * only helps if the return is checked. Here the mistake never reaches flash.
 *
 * Which to use:
 *
 *   DualSense32 + supports()   one binary, either pad, checked at run time
 *   DualShock4                 one pad, checked at compile time
 *
 * Wiring and board settings are the same as the DualShock4 example: pad in
 * the native USB port, log on the UART port, USB CDC On Boot left Disabled.
 */

#include <DualShock4.h>
#include <ds_link_usb_hid.h>

DualShock4 pad(ds_link_usb_hid());

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("DualShock4Only: plug a PS4 pad into the native USB port");
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
    if (!said) { Serial.println("that is a DualSense — wrong sketch"); said = true; }
    return;
  }
  if (!pad.connected()) return;

  if (pad.pressed(DS_CROSS))    pad.lightbar(0, 0, 255);
  if (pad.pressed(DS_CIRCLE))   pad.lightbar(255, 0, 0);
  if (pad.pressed(DS_TRIANGLE)) pad.lightbar(0, 255, 0);

  pad.rumble(pad.l2(), pad.r2());   /* left first */

  /* This one is a DualShock 4 speciality: the pad blinks the light bar on its
   * own, so it keeps blinking while the sketch is busy elsewhere. */
  if (pad.pressed(DS_PS)) {
    static bool on = false;
    on = !on;
    pad.lightbarBlink(on ? 20 : 0, on ? 20 : 0);
  }

  /* None of these will build. That is the point of this example.
   *
   * uint8_t block[DS5_TRIGGER_BLOCK_SIZE];
   * ds5_trigger_weapon(block, 3, 6, 8);
   * pad.triggerRight(block);       // no adaptive triggers
   * pad.micLed(2);                 // no microphone
   * pad.down(DS4_MUTE);            // no such button, and no such name
   * pad.down(DS5_CROSS);           // the other pad's name for a shared button
   */
}

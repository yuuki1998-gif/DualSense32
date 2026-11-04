/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * DualShock4.h — the same pad, named at compile time.
 *
 *     #include <DualShock4.h>
 *     DualShock4 pad(ds_link_usb_hid());
 *
 * Use this instead of DualSense32 when the sketch is written for a PS4 pad and
 * nothing else. The difference is entirely in what will not compile:
 *
 *     pad.triggerRight(block);   // error: use of deleted function
 *     pad.micLed(2);             // error: use of deleted function
 *     pad.playerLeds(10101);     // error: use of deleted function
 *
 * A DualShock 4 has no adaptive triggers, no microphone and no lamps under the
 * touchpad. Through DualSense32 those calls return false at run time, which
 * only helps if somebody checks; here they never build. The compiler names the
 * exact line, before anything is flashed.
 *
 * (A #define in the sketch cannot do this job. The Arduino builder compiles a
 * library's .cpp files on their own, so a macro set in the .ino never reaches
 * them. Only a type crosses that boundary.)
 *
 * What you give up: one binary that takes whichever pad is plugged in. If that
 * is what you want, keep DualSense32 and ask pad.supports(...) instead.
 *
 * Plugging in the wrong pad is still checked, at run time, because it has to
 * be — a USB port cannot be asked what will be plugged into it later.
 * connected() stays false for a DualSense, and mismatched() says why, rather
 * than decoding its reports as if they were a DualShock 4's.
 */
#ifndef DUALSHOCK4_H
#define DUALSHOCK4_H

#include "DualSense32.h"

/* The buttons a DualShock 4 actually has, with the names printed on it.
 *
 * Same underlying values as DSButton, so nothing is converted at run time —
 * this exists for what it leaves out and what it renames. There is no
 * DS4_MUTE, because there is no microphone and no button for one, and asking
 * for it is an undeclared identifier rather than a call that quietly answers
 * false for ever.
 *
 * SHARE is what Sony wrote on the pad; CREATE is the DualSense's name for the
 * button in that position. */
enum DS4Button {
  DS4_UP = DS_UP, DS4_RIGHT = DS_RIGHT, DS4_DOWN = DS_DOWN,
  DS4_LEFT = DS_LEFT,
  DS4_SQUARE = DS_SQUARE, DS4_CROSS = DS_CROSS,
  DS4_CIRCLE = DS_CIRCLE, DS4_TRIANGLE = DS_TRIANGLE,
  DS4_L1 = DS_L1, DS4_R1 = DS_R1,
  DS4_L2_CLICK = DS_L2_CLICK, DS4_R2_CLICK = DS_R2_CLICK,
  DS4_SHARE = DS_CREATE, DS4_OPTIONS = DS_OPTIONS,
  DS4_L3 = DS_L3, DS4_R3 = DS_R3,
  DS4_PS = DS_PS, DS4_TOUCHPAD = DS_TOUCHPAD,
};

class DualShock4 : public DualSense32 {
 public:
  explicit DualShock4(ds_transport_t *link = NULL) : DualSense32(link) {}

  /* Starts having already said which pad to expect, because the type said it.
   * Nothing extra to remember, and no second place for the answer to differ
   * from the first.
   *
   *   pad.begin();                          // whichever DualShock 4 turns up
   *   pad.begin("AA:BB:CC:DD:EE:FF");       // that one
   *
   * The address works here exactly as it does on DualSense32; naming a pad
   * and naming a controller model are separate things.
   *
   * The expectation only fills the window before the pad names itself, and
   * only for output: the report that arrives during that window is 0x01,
   * whose layout is the same on both pads, so reads are already right either
   * way. Writes are not -- the two build different reports -- and without an
   * expectation they are held until the pad speaks.
   *
   * connected() still waits for the pad's own word, on purpose: an
   * expectation that a DualSense could contradict is not something to send
   * DualShock 4 output on.
   *
   * DualSense32::begin(ds_device_t, ...) is deliberately hidden. On this type
   * the model is not the sketch's to choose twice. */
  bool begin(const char *addr = NULL) {
    return DualSense32::begin(DS_DEVICE_DUALSHOCK4, addr);
  }

  /* True only once a DualShock 4 is actually on the other end. A DualSense on
   * this sketch's link reports false here and true from mismatched(). */
  bool connected() const {
    return DualSense32::connected() && deviceNamed() && isDualShock4();
  }

  /* Something is connected, and it is not a DualShock 4. Worth printing: the
   * symptom otherwise is a sketch that runs and never sees a button. */
  bool mismatched() const {
    return DualSense32::connected() && deviceNamed() && !isDualShock4();
  }

  /* Connected, and still nobody has said which pad it is. Neither of the two
   * above is true here. Over Bluetooth this is normal for a moment after
   * connecting; if it never clears, the pad is still sending report 0x01 and
   * nothing has asked it for a feature report. */
  bool identifying() const {
    return DualSense32::connected() && !deviceNamed();
  }

  /* --- buttons ------------------------------------------------------------
   *
   * DSButton for ordinary sketches, DS4Button when the pad's printed names
   * matter. Naming a DualSense-only button here is a build error, which is the
   * point: DS5_MUTE does not exist on this pad, and neither does the hardware
   * behind it.
   *
   * The `using` lines matter more than they look. Declaring any down() at all
   * hides *every* inherited one, so without them nothing would compile — not
   * only the button that was meant to be refused. Learned the hard way.
   * ------------------------------------------------------------------- */
  bool down(DS4Button b) const { return DualSense32::down((DS5Button)b); }
  bool pressed(DS4Button b) const { return DualSense32::pressed((DS5Button)b); }
  bool released(DS4Button b) const {
    return DualSense32::released((DS5Button)b);
  }

  bool down(DSButton b) const { return DualSense32::down(b); }
  bool pressed(DSButton b) const { return DualSense32::pressed(b); }
  bool released(DSButton b) const { return DualSense32::released(b); }

  /* Reaching for a DualSense button on a DualShock 4 is a mistake worth
   * catching in a pad-specific sketch. Use DS_* for shared buttons. */
  bool down(DS5Button b) const = delete;
  bool pressed(DS5Button b) const = delete;
  bool released(DS5Button b) const = delete;

  /* ---------------------------------------------------------------------
   * Hardware this controller does not have.
   *
   * Deleted rather than absent, so the error names the call. Inheriting them
   * silently would put a run-time false where a build failure belongs.
   * ------------------------------------------------------------------- */
  /* Not hardware, but the same reasoning: the model is already decided by the
   * type, and a second place to state it is a second place to state it wrong.
   * Deleted rather than merely hidden so the error names begin() instead of
   * complaining that ds_device_t is not a const char*. */
  bool begin(ds_device_t expect, const char *addr = NULL) = delete;

  bool triggerLeft(const uint8_t block[DS5_TRIGGER_BLOCK_SIZE]) = delete;
  bool triggerRight(const uint8_t block[DS5_TRIGGER_BLOCK_SIZE]) = delete;
  bool micLed(uint8_t mode) = delete;
  bool playerLeds(uint16_t pattern, uint8_t brightness = 2) = delete;
  bool playerLedsMask(uint8_t mask, uint8_t brightness = 2) = delete;
};

#endif /* DUALSHOCK4_H */

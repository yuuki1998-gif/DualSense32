/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * DualSense32.h — the Arduino face of this library.
 *
 * Everything here is a thin layer over the C API in ds5_*.h, which is still
 * there and still the whole story. What this adds is the four things every
 * sketch was writing for itself:
 *
 *   - a snapshot, so loop() reads a state that is not being written to
 *   - button edges, rather than "is it down right now"
 *   - a stick deadzone
 *   - the output report, assembled and signed
 *
 * The first of those is not convenience. Reports arrive on the link's own
 * task, so a sketch that parses them straight into a global and reads it from
 * loop() is racing itself: the values are small enough that the damage is
 * usually a stick from one frame beside a button from another, but nothing
 * stops it. update() takes a copy with the link held off, and everything you
 * read afterwards came from one report.
 *
 * Which link is underneath is a constructor argument, so the same sketch runs
 * over a dongle, a cable, or an original ESP32's own radio:
 *
 *   DualSense32 pad;                          // dongle (S3/S2)
 *   DualSense32 pad(ds_link_usb_hid());      // pad on a cable
 *   DualSense32 pad(ds_bt_bluedroid());      // original ESP32
 */
#ifndef DUALSENSE32_H
#define DUALSENSE32_H

#include <Arduino.h>

#include "ds_protocol.h"
#include "ds_transport.h"

/* Buttons common to both pads. Use these in sketches unless the button only
 * exists on one controller. CREATE names the position used by CREATE on a
 * DualSense and SHARE on a DualShock 4. The two trigger entries are the click
 * at the bottom of the travel; the pressure is an axis, l2() and r2(). */
/* What one count means, for anyone doing the arithmetic by hand. The two
 * sensors do not share a scale and assuming they do is a popular way to get
 * motion that looks plausible and is sixty times wrong. */
#define DS_GYRO_LSB_PER_DPS 1024.0f
#define DS_ACCEL_LSB_PER_G  8192.0f

enum DSButton {
  DS_UP = 0, DS_RIGHT, DS_DOWN, DS_LEFT,
  DS_SQUARE, DS_CROSS, DS_CIRCLE, DS_TRIANGLE,
  DS_L1, DS_R1, DS_L2_CLICK, DS_R2_CLICK,
  DS_CREATE, DS_OPTIONS, DS_L3, DS_R3,
  DS_PS, DS_TOUCHPAD,
  DS_BUTTON_COUNT
};

/* DualSense-only names are kept for sketches that want to say exactly which
 * pad they are written for. Shared DS5_* values are aliases of DS_*; the mute
 * button is the only button here that is not common to a DualShock 4. */
enum DS5Button {
  DS5_UP = DS_UP, DS5_RIGHT = DS_RIGHT, DS5_DOWN = DS_DOWN,
  DS5_LEFT = DS_LEFT,
  DS5_SQUARE = DS_SQUARE, DS5_CROSS = DS_CROSS,
  DS5_CIRCLE = DS_CIRCLE, DS5_TRIANGLE = DS_TRIANGLE,
  DS5_L1 = DS_L1, DS5_R1 = DS_R1,
  DS5_L2_CLICK = DS_L2_CLICK, DS5_R2_CLICK = DS_R2_CLICK,
  DS5_CREATE = DS_CREATE, DS5_OPTIONS = DS_OPTIONS,
  DS5_L3 = DS_L3, DS5_R3 = DS_R3,
  DS5_PS = DS_PS, DS5_TOUCHPAD = DS_TOUCHPAD,
  DS5_MUTE = DS_BUTTON_COUNT,
  DS5_BUTTON_COUNT
};

class DualSense32 {
 public:
  /* NULL picks the only link this chip can run: the dongle on an S3 or S2,
   * the built-in radio on an original ESP32. */
  explicit DualSense32(ds_transport_t *link = NULL);

  /* addr names one controller, written the way it is read off a screen:
   * "AA:BB:CC:DD:EE:FF", dashes accepted. NULL takes whichever bonded pad
   * presses PS, which is what most sketches want.
   *
   * Returns false if the link will not start or the address is not one. */
  bool begin(const char *addr = NULL);

  /* Same, having said which pad to expect.
   *
   *   pad.begin(DS_DEVICE_DUALSHOCK4);
   *   pad.begin(DS_DEVICE_DUALSHOCK4, "AA:BB:CC:DD:EE:FF");
   *
   * Worth saying over Bluetooth, where the pad is not named until its first
   * full report: without it, anything set in that window is held back until
   * the pad speaks, because building it needs to know which dialect to build.
   * With it, output goes out correctly from the first frame.
   *
   * It is an expectation, not an override. Whatever the link works out for
   * itself replaces it, and if the pad turns out to be the other one,
   * device() says so and DualShock4::mismatched() reports it. To override a
   * product id -- for a clone that reports someone else's -- use
   * ds_link_usb_hid_force_device() instead.
   *
   * A DualShock4 does this for you: its begin() expects a DualShock 4. */
  bool begin(ds_device_t expect, const char *addr = NULL);

  void end();

  /* Call once at the top of loop(). Pumps the link, takes the snapshot
   * everything below reads from, and sends anything that has been set since
   * the last time. Without it nothing ever changes in either direction. */
  void update();

  bool connected() const { return state_ == DS_CONNECTED; }

  /* Which controller is on the other end, once it is connected.
   *
   * Over USB it comes from the product id, because both pads answer to input
   * report 0x01 at 64 bytes and the report cannot separate them. Over
   * Bluetooth it comes from the report id itself — 0x31 is a DualSense, 0x11
   * a DualShock 4 — so it is only known from the first full report, and a
   * link that has seen nothing but 0x01 still says DS_DEVICE_DUALSENSE.
   *
   * Prefer supports() to testing this: it says what the sketch needs rather
   * than what it has heard of. */
  ds_device_t device() const;

  /* Whether device() is an answer rather than a default. False until the pad
   * has said — immediately on USB, but only with the first full report over
   * Bluetooth. Anything that behaves differently per controller should wait
   * for this instead of trusting the default. */
  bool deviceNamed() const;

  /* Whether begin() was told which pad to expect. Not the same as knowing:
   * this is the sketch's word, deviceNamed() is the pad's. */
  bool deviceExpected() const;

  bool isDualShock4() const { return device() == DS_DEVICE_DUALSHOCK4; }

  /* What this pad can do, as DS_CAP_* flags. Prefer this over asking which
   * controller it is — it says what the sketch needs rather than what it has
   * heard of, and it keeps working when a pad this code predates turns up.
   *
   *   if (pad.supports(DS_CAP_ADAPTIVE_TRIGGERS)) pad.triggerRight(block);
   *
   * Only meaningful once connected. Before that the answer is the DualSense
   * set, because that is what an unidentified pad is assumed to be. */
  uint16_t capabilities() const;
  bool supports(uint16_t caps) const { return (capabilities() & caps) == caps; }
  ds_conn_state_t linkState() const { return state_; }

  /* --- buttons ---------------------------------------------------------- */

  bool down(DSButton b) const { return down((DS5Button)b); }
  bool pressed(DSButton b) const { return pressed((DS5Button)b); }
  bool released(DSButton b) const { return released((DS5Button)b); }

  bool down(DS5Button b) const;      /* held, this frame                    */
  bool pressed(DS5Button b) const;   /* went down between the last two      */
  bool released(DS5Button b) const;

  /* --- sticks and triggers ---------------------------------------------- */

  /* Centred on zero, -128..127, with the deadzone applied. Raw is in raw(). */
  int leftX() const, leftY() const, rightX() const, rightY() const;

  uint8_t l2() const { return now_.analog.l2; }
  uint8_t r2() const { return now_.analog.r2; }

  /* A resting stick wanders a few counts. Reading one without this is the
   * usual reason a robot creeps with nobody touching it. Zero disables. */
  void deadzone(uint8_t counts) { deadzone_ = counts; }

  /* --- everything else -------------------------------------------------- */

  uint8_t battery() const { return now_.status.battery; }
  bool charging() const { return now_.status.charging == 1; }

  /* Which controller this is, written the way it is read off a screen:
   * "24:A6:FA:23:12:64". Needs an 18 byte buffer. False when nothing is
   * connected, or when the link cannot say — a pad on a wire only knows its
   * own address once it has answered for it. */
  bool address(char out[18]) const;

  /* Motion, touchpad, timestamps and anything else this class does not wrap.
   * Calibration is already applied to the motion if the pad supplied it. */
  const ds_state_t &raw() const { return now_; }

  /* --- motion, in units you can reason about ------------------------------
   *
   * raw().motion holds LSB counts, and the two sensors are not on the same
   * scale: 1024 counts per deg/s against 8192 counts per g. These divide by
   * the right one.
   */
  float gyroPitch() const;   /* deg/s, nose up positive   */
  float gyroYaw() const;
  float gyroRoll() const;

  float accelX() const;      /* g, and at rest one axis reads about 1.0 */
  float accelY() const;
  float accelZ() const;

  /* Exactly what the controller sent, before the factory calibration was
   * applied — for checking the calibration itself, or for doing the scaling
   * some other way. The calibrated numbers stay available in raw(); this is
   * an extra, not a mode. */
  const ds_motion_t &rawMotion() const { return nowRaw_; }

  /* --- output ------------------------------------------------------------
   *
   * These remember what they were told. send() writes the whole state every
   * time rather than only what changed, because reports do go missing and a
   * light bar that latched a colour it never received stays wrong until
   * something else happens to set it. */

  void lightbar(uint8_t r, uint8_t g, uint8_t b);
  void rumble(uint8_t left, uint8_t right);

  /* DualShock 4 only: let the pad blink the light bar by itself, in units of
   * 10 ms. A DualSense ignores it, so guarding the call is optional — but
   * nothing happens there, and a sketch that wants to blink on both has to do
   * it in its own loop.
   *
   * Never pass an off of zero.
   *
   * The pad takes a new colour or a new rate only at a blink boundary, and an
   * off of zero leaves it with no boundary to arrive at: the light bar holds
   * whatever it was showing and ignores what comes after until something else
   * disturbs it. Both zero is worse — that puts the bar out. Neither is what
   * "solid" wants.
   *
   * For a bar that reads as steady, use a long on and an off of 1:
   *
   *     pad.lightbarBlink(200, 1);   // 2 s lit, 10 ms dark
   *
   * Measured on a DualShock 4: 10 ms of dark cannot be seen and 20 ms can, and
   * with off at 1 one report changes both the colour and the rate first time.
   * With off at 0 it took a second report seconds later to do either. */
  bool lightbarBlink(uint8_t on, uint8_t off);

  /* The five lamps under the touchpad, written the way they look:
   *
   *   pad.playerLeds(10101);   //  * . * . *
   *   pad.playerLeds(  100);   //  . . * . .   leading zeros are optional
   *   pad.playerLeds(    0);   //  all off
   *
   * One digit per lamp, left to right. Any non-zero digit lights it.
   * brightness is 0-2. */
  bool playerLeds(uint16_t pattern, uint8_t brightness = 2);

  /* The same five lamps as bits, low bit rightmost, for code that already
   * has a mask to hand. */
  bool playerLedsMask(uint8_t mask, uint8_t brightness = 2);

  bool micLed(uint8_t mode);   /* 0 off, 1 on, 2 pulsing */

  /* Blocks from ds5_trigger_off / _feedback / _weapon / _vibration. */
  bool triggerLeft(const uint8_t block[DS5_TRIGGER_BLOCK_SIZE]);
  bool triggerRight(const uint8_t block[DS5_TRIGGER_BLOCK_SIZE]);

  /* --- calls this pad could not honour ------------------------------------
   *
   * The setters above that touch optional hardware — triggerLeft,
   * triggerRight, micLed, playerLeds, playerLedsMask and lightbarBlink —
   * return false when the connected controller has no such hardware, and
   * change nothing. Adaptive triggers on a DualShock 4, a blinking light bar
   * on a DualSense. Ignoring that return is easy, and a
   * dropped field looks exactly like one that arrived, so the refusals are
   * also counted here and printEvents() reports them.
   *
   * Nothing is refused before the link is up: until a pad identifies itself
   * there is nothing to refuse against. */
  uint16_t refusedCount() const { return refused_; }
  const char *lastRefused() const { return lastRefused_; }
  void clearRefused() { refused_ = 0; lastRefused_ = nullptr; }

  /* Send now rather than waiting for the next update(). Rarely needed: the
   * setters above mark the state as changed and update() carries it out.
   * The Bluetooth sequence tag and its CRC are handled here; getting that
   * wrong is otherwise a good way to have the pad ignore every frame after
   * the first. */
  bool send();

  /* How often update() is allowed to send, in milliseconds. The pad holds
   * whatever it was last told, so there is nothing to gain from going faster
   * than the eye and the motors can follow. Zero sends every update(). */
  void sendInterval(uint16_t ms) { interval_ = ms; }

  /* --- watching it ------------------------------------------------------
   *
   * What anybody wires up first, so it is here rather than copied out of an
   * example. Prints to anything: Serial, a second UART, a network stream.
   */

  /* One short line per thing that changes, and silence otherwise. Buttons
   * and fingers are reported the frame they happen; sticks, triggers and a
   * dragging finger are slowed to a few times a second so the events stay
   * readable. Call it from loop(), after update(). */
  void printEvents(Print &out);

  /* Everything at once, including the motion sensors that printEvents leaves
   * alone. For a key that dumps the lot on demand. */
  void printState(Print &out);

  static const char *buttonName(DSButton b) { return buttonName((DS5Button)b); }
  static const char *buttonName(DS5Button b);

  /* The transport, for the link-specific calls this class does not wrap:
   * bond storage, the peer address, the connection log. */
  ds_transport_t *transport() const { return tr_; }

 private:
  static void reportThunk(const uint8_t *report, uint16_t len, void *user);
  static void stateThunk(ds_conn_state_t st, void *user);

  /* Everything both begin()s do once the expectation is decided. */
  bool start(const char *addr);

  /* True if this pad has `cap`; otherwise counts a refusal and returns false. */
  bool allow(uint16_t cap, const char *what);

  /* False when this pad has no such button, counting a refusal as it goes.
   * ...Quiet asks the same question without counting, for the library's own
   * sweeps, which are enumeration rather than a sketch reaching for it. */
  bool haveButton(DS5Button b) const;
  bool haveButtonQuiet(DS5Button b) const;

  ds_transport_t *tr_;
  ds_state_t now_, was_, incoming_;
  ds_motion_t nowRaw_, incomingRaw_;   /* before calibration */
  ds_output_t out_;
  ds_conn_state_t state_;
  uint8_t deadzone_;
  uint8_t sequence_;
  bool pending_;          /* something was set and has not gone out yet */
  uint16_t interval_;
  uint32_t lastSend_;
  volatile bool fresh_;
  portMUX_TYPE lock_;

  /* Mutable: the const query methods report refusals too. */
  mutable uint16_t refused_ = 0;
  mutable const char *lastRefused_ = nullptr;

  /* printEvents() remembers what it has already said. */
  struct {
    bool started;
    bool linked;
    bool finger[2];
    ds_status_t status;
    uint32_t axesAt, dragAt;
    /* The call last refused, by pointer: these are string literals with
     * static storage, so comparing the pointer is comparing the call. */
    const char *refused;
  } said_;
};

#endif /* DUALSENSE32_H */

/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * DualSense32.cpp — see DualSense32.h.
 */
#include "DualSense32.h"

#include <string.h>

#include "ds_hci.h"       /* only for the address parser */
#include "ds_usb_hci.h"   /* decides DS_HAS_USB_HCI, so it comes first */

#ifdef DS_HAS_USB_HCI
#include "ds_link_usb_hci.h"
#else
#include "ds_bt_bluedroid.h"
#endif

/* ========================================================================== */
/*                              construction                                  */
/* ========================================================================== */

DualSense32::DualSense32(ds_transport_t *link)
    : tr_(link),
      state_(DS_DISCONNECTED),
      deadzone_(8),
      sequence_(0),
      pending_(false),
      interval_(50),
      lastSend_(0),
      fresh_(false) {
  portMUX_TYPE unlocked = portMUX_INITIALIZER_UNLOCKED;
  lock_ = unlocked;
  memset(&now_, 0, sizeof(now_));
  memset(&was_, 0, sizeof(was_));
  memset(&incoming_, 0, sizeof(incoming_));
  memset(&nowRaw_, 0, sizeof(nowRaw_));
  memset(&incomingRaw_, 0, sizeof(incomingRaw_));
  ds_output_init(&out_);

  if (tr_ == NULL) {
    /* The only link this chip can run. An S3 has no Bluetooth Classic radio,
     * so there the dongle is the radio; an original ESP32 has one and no USB
     * host controller to drive a dongle with. */
#ifdef DS_HAS_USB_HCI
    tr_ = ds_link_usb_hci();
#else
    tr_ = ds_bt_bluedroid();
#endif
  }
}

/* ========================================================================== */
/*                          the link's side                                   */
/* ========================================================================== */

/*
 * Both of these run on the link's task, not on loop(). Parsing happens on this
 * side of the fence and only the copy is guarded, so the link is held off for
 * a memcpy rather than for the whole decode.
 */
void DualSense32::reportThunk(const uint8_t *report, uint16_t len, void *user) {
  DualSense32 *self = (DualSense32 *)user;
  ds_state_t parsed;

  memset(&parsed, 0, sizeof(parsed));
  ds_parse_input_dev(report, len, self->tr_->link, self->tr_->device,
                      &parsed);

  /* Kept before scaling, because calibration cannot be undone afterwards:
   * the gyro divisor differs from controller to controller. */
  ds_motion_t untouched = parsed.motion;

  if (self->tr_->calibration != NULL) {
    ds_apply_calibration(self->tr_->calibration(self->tr_), &parsed.motion);
  }

  portENTER_CRITICAL(&self->lock_);
  self->incoming_ = parsed;
  self->incomingRaw_ = untouched;
  self->fresh_ = true;
  portEXIT_CRITICAL(&self->lock_);
}

void DualSense32::stateThunk(ds_conn_state_t st, void *user) {
  ((DualSense32 *)user)->state_ = st;
}

/* ========================================================================== */
/*                            starting and stopping                           */
/* ========================================================================== */

bool DualSense32::begin(const char *addr) {
  if (tr_ == NULL) return false;
  tr_->device_expected = false;
  return start(addr);
}

bool DualSense32::begin(ds_device_t expect, const char *addr) {
  if (tr_ == NULL) return false;
  tr_->device_expect = expect;
  tr_->device_expected = true;
  return start(addr);
}

/* The expectation is set before this runs, because forgetting the last pad is
 * what seeds `device` from it. */
bool DualSense32::start(const char *addr) {
  uint8_t bd[DS_HCI_BD_ADDR_LEN];

  if (tr_->begin == NULL) return false;

  tr_->on_report = &DualSense32::reportThunk;
  tr_->on_state = &DualSense32::stateThunk;
  tr_->user = this;
  ds_transport_forget_device(tr_);

  if (addr == NULL) return tr_->begin(tr_, NULL);
  /* A mistyped address is six bytes like any other, and the only symptom
   * would be a host that pages nothing for ever. Fail here instead. */
  if (!ds_hci_str_to_addr(addr, bd)) return false;
  return tr_->begin(tr_, bd);
}

bool DualSense32::address(char out[18]) const {
  const uint8_t *bd;

  if (out == NULL || tr_ == NULL || tr_->peer == NULL) return false;
  bd = tr_->peer(tr_);
  if (bd == NULL) return false;

  ds_hci_addr_to_str(bd, out);
  return true;
}

void DualSense32::end() {
  if (tr_ != NULL && tr_->end != NULL) tr_->end(tr_);
  state_ = DS_DISCONNECTED;
}

/* ========================================================================== */
/*                                the snapshot                                */
/* ========================================================================== */

void DualSense32::update() {
  if (tr_ == NULL) return;
  if (tr_->poll != NULL) tr_->poll(tr_);

  /* Last frame's buttons are what this frame's edges are measured against, so
   * they have to be kept before the new ones land. */
  was_ = now_;

  portENTER_CRITICAL(&lock_);
  if (fresh_) {
    now_ = incoming_;
    nowRaw_ = incomingRaw_;
    fresh_ = false;
  }
  portEXIT_CRITICAL(&lock_);

  /* Anything set since the last time goes out here, so that setting it was
   * enough. A sketch that would rather choose the moment calls send() and
   * clears the flag itself. */
  if (pending_ && (uint32_t)(millis() - lastSend_) >= interval_) send();
}

/* ========================================================================== */
/*                                  buttons                                   */
/* ========================================================================== */

/* One place that knows the order of the enum. A switch rather than an array
 * because ds_buttons_t is a struct of named flags, and reading a struct as
 * an array is the kind of shortcut that works until a field is added. */
static bool pick(const ds_buttons_t &b, DS5Button which) {
  switch (which) {
    case DS5_UP:       return b.up;
    case DS5_RIGHT:    return b.right;
    case DS5_DOWN:     return b.down;
    case DS5_LEFT:     return b.left;
    case DS5_SQUARE:   return b.square;
    case DS5_CROSS:    return b.cross;
    case DS5_CIRCLE:   return b.circle;
    case DS5_TRIANGLE: return b.triangle;
    case DS5_L1:       return b.l1;
    case DS5_R1:       return b.r1;
    case DS5_L2_CLICK: return b.l2;
    case DS5_R2_CLICK: return b.r2;
    case DS5_CREATE:   return b.create;
    case DS5_OPTIONS:  return b.options;
    case DS5_L3:       return b.l3;
    case DS5_R3:       return b.r3;
    case DS5_PS:       return b.ps;
    case DS5_TOUCHPAD: return b.touchpad;
    case DS5_MUTE:     return b.mute;
    default:           return false;
  }
}

/* A button this pad does not have reads false for ever, and a sketch polling
 * it is indistinguishable from one whose user never pressed it. The API cannot
 * catch that at build time — a button is a value here, not a type — so it goes
 * through the same refusal count as the output calls, and printEvents() says
 * it once. */
bool DualSense32::haveButtonQuiet(DS5Button b) const {
  if (b != DS5_MUTE) return true;
  return !connected() || supports(DS_CAP_MUTE_BUTTON);
}

bool DualSense32::haveButton(DS5Button b) const {
  if (haveButtonQuiet(b)) return true;
  if (refused_ != 0xFFFF) refused_++;
  lastRefused_ = "DS5_MUTE";
  return false;
}

bool DualSense32::down(DS5Button b) const {
  if (!haveButton(b)) return false;
  return pick(now_.button, b);
}

bool DualSense32::pressed(DS5Button b) const {
  if (!haveButton(b)) return false;
  return pick(now_.button, b) && !pick(was_.button, b);
}

bool DualSense32::released(DS5Button b) const {
  if (!haveButton(b)) return false;
  return !pick(now_.button, b) && pick(was_.button, b);
}

/* ========================================================================== */
/*                                   sticks                                   */
/* ========================================================================== */

/* Raw sticks rest near 128 rather than exactly on it, and wander a little
 * either side. Centring without a deadzone gives a small permanent reading. */
static int centred(uint8_t raw, uint8_t dead) {
  int v = (int)raw - 128;
  if (v > -(int)dead && v < (int)dead) return 0;
  return v;
}

int DualSense32::leftX() const  { return centred(now_.analog.lx, deadzone_); }
int DualSense32::leftY() const  { return centred(now_.analog.ly, deadzone_); }
int DualSense32::rightX() const { return centred(now_.analog.rx, deadzone_); }
int DualSense32::rightY() const { return centred(now_.analog.ry, deadzone_); }

/* ========================================================================== */
/*                                   output                                   */
/* ========================================================================== */

void DualSense32::lightbar(uint8_t r, uint8_t g, uint8_t b) {
  pending_ = true;
  out_.set_lightbar = true;
  out_.red = r;
  out_.green = g;
  out_.blue = b;
}

bool DualSense32::lightbarBlink(uint8_t on, uint8_t off) {
  if (!allow(DS_CAP_LIGHTBAR_BLINK, "lightbarBlink")) return false;
  pending_ = true;
  out_.set_lightbar_blink = true;
  out_.blink_on = on;
  out_.blink_off = off;
  return true;
}

ds_device_t DualSense32::device() const {
  return tr_ != NULL ? tr_->device : DS_DEVICE_DUALSENSE;
}

bool DualSense32::deviceNamed() const {
  return tr_ != NULL && tr_->device_named;
}

bool DualSense32::deviceExpected() const {
  return tr_ != NULL && tr_->device_expected;
}

uint16_t DualSense32::capabilities() const {
  return ds_capabilities(device());
}

/* Gate for hardware not every pad has. Refusing before the link is up would
 * only be guessing: the device is not known until a pad identifies itself, so
 * anything set in setup() goes through and is judged when it is sent. */
bool DualSense32::allow(uint16_t cap, const char *what) {
  if (!connected() || supports(cap)) return true;
  if (refused_ != 0xFFFF) refused_++;
  lastRefused_ = what;
  return false;
}

void DualSense32::rumble(uint8_t left, uint8_t right) {
  pending_ = true;
  out_.set_rumble = true;
  out_.rumble_left = left;
  out_.rumble_right = right;
}

bool DualSense32::playerLedsMask(uint8_t mask, uint8_t brightness) {
  if (!allow(DS_CAP_PLAYER_LEDS, "playerLeds")) return false;
  pending_ = true;
  out_.set_player_leds = true;
  out_.player_leds = (uint8_t)(mask & 0x1F);
  out_.led_brightness = brightness;
  return true;
}

bool DualSense32::playerLeds(uint16_t pattern, uint8_t brightness) {
  /* Rightmost digit is the rightmost lamp, which is bit zero. Reading the
   * digits off the low end means 100 and 00100 mean the same thing. */
  uint8_t mask = 0;
  for (int lamp = 0; lamp < 5 && pattern != 0; lamp++) {
    if (pattern % 10 != 0) mask |= (uint8_t)(1u << lamp);
    pattern /= 10;
  }
  return playerLedsMask(mask, brightness);
}

/* ========================================================================== */
/*                                  motion                                    */
/* ========================================================================== */

float DualSense32::gyroPitch() const { return now_.motion.gyro[0] / DS_GYRO_LSB_PER_DPS; }
float DualSense32::gyroYaw() const   { return now_.motion.gyro[1] / DS_GYRO_LSB_PER_DPS; }
float DualSense32::gyroRoll() const  { return now_.motion.gyro[2] / DS_GYRO_LSB_PER_DPS; }

float DualSense32::accelX() const { return now_.motion.accel[0] / DS_ACCEL_LSB_PER_G; }
float DualSense32::accelY() const { return now_.motion.accel[1] / DS_ACCEL_LSB_PER_G; }
float DualSense32::accelZ() const { return now_.motion.accel[2] / DS_ACCEL_LSB_PER_G; }

bool DualSense32::micLed(uint8_t mode) {
  if (!allow(DS_CAP_MIC_LED, "micLed")) return false;
  pending_ = true;
  out_.set_mic_led = true;
  out_.mic_led = mode;
  return true;
}

bool DualSense32::triggerLeft(const uint8_t block[DS5_TRIGGER_BLOCK_SIZE]) {
  if (!allow(DS_CAP_ADAPTIVE_TRIGGERS, "triggerLeft")) return false;
  pending_ = true;
  out_.set_trigger_left = true;
  memcpy(out_.trigger_left, block, DS5_TRIGGER_BLOCK_SIZE);
  return true;
}

bool DualSense32::triggerRight(const uint8_t block[DS5_TRIGGER_BLOCK_SIZE]) {
  if (!allow(DS_CAP_ADAPTIVE_TRIGGERS, "triggerRight")) return false;
  pending_ = true;
  out_.set_trigger_right = true;
  memcpy(out_.trigger_right, block, DS5_TRIGGER_BLOCK_SIZE);
  return true;
}

bool DualSense32::send() {
  uint8_t buf[DS_OUT_MAX_SIZE];
  uint16_t n;

  if (tr_ == NULL || tr_->send == NULL) return false;
  if (state_ != DS_CONNECTED) return false;

  /* Which report to build depends on which pad this is, and over Bluetooth
   * that is unknown between connecting and the first full report. Building on
   * the default puts a DualSense report on a DualShock 4, which the pad
   * ignores -- and because the caller's flag is cleared below, the setting
   * would be lost rather than delivered late. So hold: pending_ stays up and
   * update() tries again, which costs a few frames once.
   *
   * begin(DS_DEVICE_...) skips the wait entirely by saying which pad to
   * expect, and on USB there is no wait to begin with. */
  if (!tr_->device_named && !tr_->device_expected) return false;

  /* The CRC covers this, so it goes in before the report is built rather than
   * being stamped into a finished one. Ignored on the USB links. */
  out_.sequence = sequence_++;

  n = ds_build_output_dev(&out_, tr_->link, tr_->device, buf, sizeof(buf));
  if (n == 0) return false;

  pending_ = false;
  lastSend_ = millis();
  return tr_->send(tr_, buf, n);
}

/* ========================================================================== */
/*                                 watching it                                */
/* ========================================================================== */

/* In the order of the enum, so printEvents can name what it finds without a
 * second switch. */
static const char *const BUTTON_NAMES[DS5_BUTTON_COUNT] = {
    "up",     "right",   "down",   "left",     "square", "cross", "circle",
    "triangle", "L1",    "R1",     "L2 click", "R2 click", "create",
    "options", "L3",     "R3",     "PS",       "touchpad", "mute",
};

const char *DualSense32::buttonName(DS5Button b) {
  return (b >= 0 && b < DS5_BUTTON_COUNT) ? BUTTON_NAMES[b] : "?";
}

void DualSense32::printEvents(Print &out) {
  const ds_state_t &s = now_;
  uint32_t at = millis();
  int i;

  if (!said_.started) {
    memset(&said_, 0, sizeof(said_));
    said_.started = true;
  }

  if (connected() != said_.linked) {
    said_.linked = connected();
    if (said_.linked) {
      /* The address is the one thing a sketch cannot work out for itself and
       * needs later, to name this pad instead of taking whichever answers. */
      char who[18];
      if (address(who)) out.printf("link: connected %s\n", who);
      else              out.println("link: connected");
    } else {
      out.println("link: gone");
    }
  }
  if (!connected()) return;

  /* A refused call is the one failure here that leaves no trace on the wire:
   * the field is simply absent, and an absent field looks like one that
   * arrived. Say it once per new refusal rather than every frame. */
  /* Keyed on which call was dropped, not on how many have been. The counter
   * rises once per refused call, and a setter called every time round loop()
   * is the normal way to write one — reporting by count would print a line a
   * frame and bury the log. The name is the useful part; refusedCount() is
   * there for anyone who wants the total. */
  if (lastRefused_ != said_.refused) {
    said_.refused = lastRefused_;
    out.printf("unsupported: %s — this pad is a %s\n",
               lastRefused_ != nullptr ? lastRefused_ : "?",
               isDualShock4() ? "DualShock 4" : "DualSense");
  }

  for (i = 0; i < DS5_BUTTON_COUNT; i++) {
    DS5Button b = (DS5Button)i;
    /* Skip rather than ask. Sweeping every button and letting the missing one
     * be refused would count a refusal per frame, and then report it per
     * frame — this loop enumerating what exists is not a sketch reaching for
     * something that does not. */
    if (!haveButtonQuiet(b)) continue;
    if (pressed(b))  out.printf("%-9s down\n", BUTTON_NAMES[i]);
    if (released(b)) out.printf("%-9s up\n", BUTTON_NAMES[i]);
  }

  /* Battery, charger, headphone jack and mic mute arrive in the same report
   * as everything else, so they are free to watch. */
  {
    const ds_status_t &st = s.status;
    const ds_status_t &was = said_.status;
    if (st.valid && (st.battery != was.battery || st.charging != was.charging ||
                     st.jack != was.jack || st.mic_muted != was.mic_muted)) {
      said_.status = st;
      /* A DualShock 4 reports neither, and "mic live" on a pad with no
       * microphone is a claim rather than a blank. */
      if (supports(DS_CAP_MUTE_BUTTON)) {
        out.printf("status battery %u/10%s  jack %s  mic %s\n", st.battery,
                   st.charging == 1 ? " charging" : "",
                   st.jack ? "in" : "out", st.mic_muted ? "muted" : "live");
      } else {
        out.printf("status battery %u/10%s\n", st.battery,
                   st.charging == 1 ? " charging" : "");
      }
    }
  }

  /* A tap is easily shorter than the throttle below, so fingers landing and
   * lifting are checked every frame. Only the dragging is slowed. */
  if (s.has_touch) {
    bool drag = (uint32_t)(at - said_.dragAt) >= 200;
    for (i = 0; i < 2; i++) {
      bool isDown = s.touch[i].active;
      if (isDown && !said_.finger[i]) {
        out.printf("touch  finger %d down  id %-3u  %4u,%4u\n", i,
                   s.touch[i].id, s.touch[i].x, s.touch[i].y);
      } else if (!isDown && said_.finger[i]) {
        out.printf("touch  finger %d up\n", i);
      } else if (isDown && drag) {
        out.printf("touch  finger %d      id %-3u  %4u,%4u\n", i,
                   s.touch[i].id, s.touch[i].x, s.touch[i].y);
      }
      said_.finger[i] = isDown;
    }
    if (drag) said_.dragAt = at;
  }

  /* Axes change on every single report, so they are printed a few times a
   * second and only while something is away from rest. */
  if ((uint32_t)(at - said_.axesAt) < 200) return;
  said_.axesAt = at;
  if (leftX() || leftY() || rightX() || rightY() || l2() || r2()) {
    out.printf("stick  L %4d,%4d   R %4d,%4d   trigger L2 %3u  R2 %3u\n",
               leftX(), leftY(), rightX(), rightY(), l2(), r2());
  }
}

void DualSense32::printState(Print &out) {
  const ds_state_t &s = now_;
  int i;

  out.printf("-- report 0x%02X  battery %u/10%s\n", s.report_id, battery(),
             charging() ? " charging" : "");
  out.printf("   L %4d,%4d   R %4d,%4d   L2 %3u  R2 %3u\n", leftX(), leftY(),
             rightX(), rightY(), l2(), r2());

  out.print("   held:");
  for (i = 0; i < DS5_BUTTON_COUNT; i++) {
    if (down((DS5Button)i)) {
      out.print(' ');
      out.print(BUTTON_NAMES[i]);
    }
  }
  out.println();

  if (s.has_touch) {
    for (i = 0; i < 2; i++) {
      out.printf("   touch %d %s id %-3u %4u,%4u\n", i,
                 s.touch[i].active ? "down" : "up  ", s.touch[i].id,
                 s.touch[i].x, s.touch[i].y);
    }
  }

  /* Calibrated, so the gyro is 1024 counts per degree per second and the
   * accelerometer 8192 per g. Raw it would be about sixteen per deg/s and
   * differ between controllers. */
  if (s.has_motion) {
    out.printf("   gyro %6ld %6ld %6ld   accel %6d %6d %6d\n",
               (long)s.motion.gyro[0], (long)s.motion.gyro[1],
               (long)s.motion.gyro[2], s.motion.accel[0], s.motion.accel[1],
               s.motion.accel[2]);
  } else {
    out.println("   no motion or touch: the pad is still sending the short "
                "compatibility report");
  }
}

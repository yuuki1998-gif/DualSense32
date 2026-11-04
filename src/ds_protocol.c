/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
#include "ds_protocol.h"

#include <string.h>

/* ========================================================================== */
/*                                  C R C 3 2                                 */
/* ========================================================================== */

#ifndef DS_CRC32_EXTERNAL
/* Bitwise so the core stays table-free and dependency-free. An output report
 * is 75 bytes, so this runs ~600 iterations per frame - negligible next to the
 * Bluetooth turnaround. Transports that care can override it. */
uint32_t ds_crc32(uint32_t crc, const uint8_t *data, uint32_t len) {
  crc = ~crc;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return ~crc;
}
#endif

/* ========================================================================== */
/*                        A D A P T I V E   T R I G G E R S                   */
/* ========================================================================== */

#define DS5_TRIG_MODE_OFF       0x05
#define DS5_TRIG_MODE_FEEDBACK  0x21
#define DS5_TRIG_MODE_WEAPON    0x25
#define DS5_TRIG_MODE_VIBRATION 0x26

void ds5_trigger_off(uint8_t block[DS5_TRIGGER_BLOCK_SIZE]) {
  memset(block, 0, DS5_TRIGGER_BLOCK_SIZE);
  block[0] = DS5_TRIG_MODE_OFF;
}

/* Feedback and Vibration share a layout: a 10-bit "which zones are active"
 * mask, then three bits of force/amplitude per zone packed little-endian. */
static void pack_zones(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t mode,
                       uint8_t position, uint8_t level) {
  uint8_t value = (uint8_t)((level - 1) & 0x07);
  uint32_t zones = 0;
  uint16_t active = 0;

  for (int i = position; i < 10; i++) {
    zones |= ((uint32_t)value) << (3 * i);
    active |= (uint16_t)(1u << i);
  }

  memset(block, 0, DS5_TRIGGER_BLOCK_SIZE);
  block[0] = mode;
  block[1] = (uint8_t)(active & 0xFF);
  block[2] = (uint8_t)(active >> 8);
  block[3] = (uint8_t)(zones & 0xFF);
  block[4] = (uint8_t)(zones >> 8);
  block[5] = (uint8_t)(zones >> 16);
  block[6] = (uint8_t)(zones >> 24);
  /* Zones 11 and up would need the 64-bit tail the controller ignores. */
}

bool ds5_trigger_feedback(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                          uint8_t position, uint8_t strength) {
  if (position > 9 || strength > 8 || strength == 0) {
    ds5_trigger_off(block);
    return false;
  }
  pack_zones(block, DS5_TRIG_MODE_FEEDBACK, position, strength);
  return true;
}

bool ds5_trigger_weapon(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                        uint8_t start, uint8_t end, uint8_t strength) {
  if (start < 2 || start > 7 || end > 8 || end <= start ||
      strength > 8 || strength == 0) {
    ds5_trigger_off(block);
    return false;
  }

  uint16_t zones = (uint16_t)((1u << start) | (1u << end));

  memset(block, 0, DS5_TRIGGER_BLOCK_SIZE);
  block[0] = DS5_TRIG_MODE_WEAPON;
  block[1] = (uint8_t)(zones & 0xFF);
  block[2] = (uint8_t)(zones >> 8);
  block[3] = (uint8_t)(strength - 1);
  return true;
}

bool ds5_trigger_vibration(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                           uint8_t position, uint8_t amplitude,
                           uint8_t frequency) {
  if (position > 9 || amplitude > 8 || amplitude == 0 || frequency == 0) {
    ds5_trigger_off(block);
    return false;
  }
  pack_zones(block, DS5_TRIG_MODE_VIBRATION, position, amplitude);
  block[9] = frequency;
  return true;
}

#define DS5_TRIG_MODE_BOW       0x22
#define DS5_TRIG_MODE_GALLOPING 0x23
#define DS5_TRIG_MODE_MACHINE   0x27

/* The two general forms. Both walk ten zones, keep the ones with a value in
 * them, and pack three bits each into a 32-bit word -- which is why zone 10
 * and beyond would need a tail the controller ignores. */
static bool pack_list(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t mode,
                      const uint8_t list[10]) {
  uint32_t packed = 0;
  uint16_t active = 0;
  int i;

  for (i = 0; i < 10; i++) {
    if (list[i] == 0) continue;
    if (list[i] > 8) return false;
    packed |= ((uint32_t)((list[i] - 1) & 0x07)) << (3 * i);
    active |= (uint16_t)(1u << i);
  }
  if (active == 0) {
    ds5_trigger_off(block);
    return false;
  }

  memset(block, 0, DS5_TRIGGER_BLOCK_SIZE);
  block[0] = mode;
  block[1] = (uint8_t)(active & 0xFF);
  block[2] = (uint8_t)(active >> 8);
  block[3] = (uint8_t)(packed & 0xFF);
  block[4] = (uint8_t)(packed >> 8);
  block[5] = (uint8_t)(packed >> 16);
  block[6] = (uint8_t)(packed >> 24);
  return true;
}

bool ds5_trigger_multi_feedback(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                                const uint8_t strength[10]) {
  if (strength == NULL) {
    ds5_trigger_off(block);
    return false;
  }
  return pack_list(block, DS5_TRIG_MODE_FEEDBACK, strength);
}

bool ds5_trigger_multi_vibration(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                                 uint8_t frequency,
                                 const uint8_t amplitude[10]) {
  if (amplitude == NULL || frequency == 0) {
    ds5_trigger_off(block);
    return false;
  }
  if (!pack_list(block, DS5_TRIG_MODE_VIBRATION, amplitude)) return false;
  block[9] = frequency;
  return true;
}

bool ds5_trigger_slope(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t start,
                       uint8_t end, uint8_t start_strength,
                       uint8_t end_strength) {
  uint8_t list[10];
  int32_t span, rise;
  int i;

  if (start > 8 || end > 9 || end <= start) {
    ds5_trigger_off(block);
    return false;
  }
  if (start_strength < 1 || start_strength > 8 || end_strength < 1 ||
      end_strength > 8) {
    ds5_trigger_off(block);
    return false;
  }

  memset(list, 0, sizeof(list));
  span = (int32_t)end - (int32_t)start;
  rise = (int32_t)end_strength - (int32_t)start_strength;
  for (i = start; i < 10; i++) {
    if (i <= end) {
      /* Rounded to nearest, away from zero. The reference rounds halves to
       * even instead; the two differ only when a zone lands exactly on a
       * half, which no useful ramp does. */
      int32_t num = rise * (int32_t)(i - start);
      int32_t half = (num >= 0) ? span / 2 : -(span / 2);
      list[i] = (uint8_t)((int32_t)start_strength + (num + half) / span);
    } else {
      list[i] = end_strength;
    }
  }
  return pack_list(block, DS5_TRIG_MODE_FEEDBACK, list);
}

/* --- the undocumented three ---------------------------------------------- */

/* All three start the same way: one bit per end of the affected span. */
static void pack_span(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t mode,
                      uint8_t start, uint8_t end) {
  uint16_t zones = (uint16_t)((1u << start) | (1u << end));
  memset(block, 0, DS5_TRIGGER_BLOCK_SIZE);
  block[0] = mode;
  block[1] = (uint8_t)(zones & 0xFF);
  block[2] = (uint8_t)(zones >> 8);
}

bool ds5_trigger_bow(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t start,
                     uint8_t end, uint8_t strength, uint8_t snap_force) {
  if (start > 8 || end > 8 || end <= start || strength == 0 || strength > 8 ||
      snap_force == 0 || snap_force > 8) {
    ds5_trigger_off(block);
    return false;
  }
  pack_span(block, DS5_TRIG_MODE_BOW, start, end);
  block[3] = (uint8_t)(((strength - 1) & 0x07) | (((snap_force - 1) & 0x07) << 3));
  return true;
}

bool ds5_trigger_galloping(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                           uint8_t start, uint8_t end, uint8_t first_foot,
                           uint8_t second_foot, uint8_t frequency) {
  if (start > 8 || end > 9 || end <= start || first_foot > 6 ||
      second_foot > 7 || first_foot >= second_foot || frequency == 0) {
    ds5_trigger_off(block);
    return false;
  }
  pack_span(block, DS5_TRIG_MODE_GALLOPING, start, end);
  block[3] = (uint8_t)((second_foot & 0x07) | ((first_foot & 0x07) << 3));
  block[4] = frequency;
  return true;
}

bool ds5_trigger_machine(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t start,
                         uint8_t end, uint8_t amplitude_a, uint8_t amplitude_b,
                         uint8_t frequency, uint8_t period) {
  if (start > 8 || end > 9 || end <= start || amplitude_a > 7 ||
      amplitude_b > 7 || frequency == 0) {
    ds5_trigger_off(block);
    return false;
  }
  pack_span(block, DS5_TRIG_MODE_MACHINE, start, end);
  block[3] = (uint8_t)((amplitude_a & 0x07) | ((amplitude_b & 0x07) << 3));
  block[4] = frequency;
  block[5] = period;
  return true;
}

/* ========================================================================== */
/*                                O U T P U T                                 */
/* ========================================================================== */

/* Offsets inside `dualsense_output_report_common`, which sits at a different
 * place on each link but always has the same internal layout. */
enum {
  CO_VALID_FLAG0 = 0,
  CO_VALID_FLAG1 = 1,
  CO_MOTOR_RIGHT = 2,
  CO_MOTOR_LEFT = 3,
  CO_MUTE_LED = 8,      /* mute_button_led; 9 is power_save_control */
  CO_R2_TRIGGER = 10,   /* 11 bytes, inside what Linux calls reserved2[27] */
  CO_L2_TRIGGER = 21,   /* 11 bytes */
  CO_VALID_FLAG2 = 38,
  CO_LIGHTBAR_SETUP = 41,
  CO_LED_BRIGHTNESS = 42,
  CO_PLAYER_LEDS = 43,
  CO_LIGHTBAR_RED = 44,
  CO_LIGHTBAR_GREEN = 45,
  CO_LIGHTBAR_BLUE = 46,
  CO_SIZE = 47,
};

/* valid_flag bits. Nothing in the report is applied unless its bit is set.
 * Rumble needs COMPATIBLE_VIBRATION *and* HAPTICS_SELECT: bit 0 on its own
 * selects the haptic actuators instead of the rumble motors. */
enum {
  VF0_COMPATIBLE_VIBRATION = 1u << 0,
  VF0_HAPTICS_SELECT = 1u << 1,
  VF0_R2_TRIGGER = 1u << 2,
  VF0_L2_TRIGGER = 1u << 3,

  VF1_MIC_MUTE_LED = 1u << 0,
  VF1_LIGHTBAR = 1u << 2,
  VF1_RELEASE_LEDS = 1u << 3,
  VF1_PLAYER_INDICATOR = 1u << 4,

  VF2_LIGHTBAR_SETUP = 1u << 1,
  VF2_COMPATIBLE_VIBRATION2 = 1u << 2,  /* preferred by firmware >= 2.21 */
};

#define LIGHTBAR_SETUP_LIGHT_OUT 0x02  /* cancels the blue power-on fade */
#define DS5_OUTPUT_TAG 0x10            /* mandatory; report dropped without it */

void ds_output_init(ds_output_t *out) {
  memset(out, 0, sizeof(*out));
  ds5_trigger_off(out->trigger_left);
  ds5_trigger_off(out->trigger_right);
}

static void fill_common(const ds_output_t *out, uint8_t *c) {
  uint8_t f0 = 0, f1 = 0, f2 = 0;

  if (out->set_rumble) {
    f0 |= VF0_COMPATIBLE_VIBRATION | VF0_HAPTICS_SELECT;
    f2 |= VF2_COMPATIBLE_VIBRATION2;
    c[CO_MOTOR_RIGHT] = out->rumble_right;
    c[CO_MOTOR_LEFT] = out->rumble_left;
  }

  if (out->set_lightbar) {
    f1 |= VF1_LIGHTBAR;
    f2 |= VF2_LIGHTBAR_SETUP;
    c[CO_LIGHTBAR_SETUP] = LIGHTBAR_SETUP_LIGHT_OUT;
    c[CO_LIGHTBAR_RED] = out->red;
    c[CO_LIGHTBAR_GREEN] = out->green;
    c[CO_LIGHTBAR_BLUE] = out->blue;
  }

  if (out->set_player_leds) {
    f1 |= VF1_PLAYER_INDICATOR;
    c[CO_PLAYER_LEDS] = out->player_leds & 0x1F;
    c[CO_LED_BRIGHTNESS] = (out->led_brightness > 2) ? 2 : out->led_brightness;
  }

  if (out->set_mic_led) {
    f1 |= VF1_MIC_MUTE_LED;
    c[CO_MUTE_LED] = out->mic_led;
  }

  if (out->set_trigger_right) {
    f0 |= VF0_R2_TRIGGER;
    memcpy(&c[CO_R2_TRIGGER], out->trigger_right, DS5_TRIGGER_BLOCK_SIZE);
  }

  if (out->set_trigger_left) {
    f0 |= VF0_L2_TRIGGER;
    memcpy(&c[CO_L2_TRIGGER], out->trigger_left, DS5_TRIGGER_BLOCK_SIZE);
  }

  c[CO_VALID_FLAG0] = f0;
  c[CO_VALID_FLAG1] = f1;
  c[CO_VALID_FLAG2] = f2;
}

/* --------------------------------------------------------- DualShock 4 --- */

/* Which of the ten common bytes the pad should act on. Everything else in a
 * DualShock 4 output report is audio, which this library does not drive. */
enum {
  DS4_VALID0_MOTOR = 0x01,
  DS4_VALID0_LED = 0x02,
  DS4_VALID0_LED_BLINK = 0x04,
};

/* The ten bytes both links share. Note the motors arrive right before left,
 * the opposite of the struct field order and of the DualSense report. */
static void fill_ds4_common(const ds_output_t *out, uint8_t *c) {
  uint8_t f0 = 0;
  if (out->set_rumble) f0 |= DS4_VALID0_MOTOR;
  if (out->set_lightbar) f0 |= DS4_VALID0_LED;
  if (out->set_lightbar_blink) f0 |= DS4_VALID0_LED_BLINK;
  c[0] = f0;
  c[1] = 0;                    /* valid_flag1 is audio only */
  c[2] = 0;                    /* reserved */
  c[3] = out->rumble_right;    /* the small, high frequency motor */
  c[4] = out->rumble_left;     /* the large, low frequency one    */
  c[5] = out->red;
  c[6] = out->green;
  c[7] = out->blue;
  c[8] = out->blink_on;
  c[9] = out->blink_off;
}

static uint16_t build_ds4_output(const ds_output_t *out, ds_link_t link,
                                 uint8_t *buf, uint16_t buflen) {
  uint32_t crc;
  uint16_t total;

  if (link == DS_LINK_USB) {
    if (buflen < DS4_OUT_USB_SIZE) return 0;
    memset(buf, 0, DS4_OUT_USB_SIZE);
    buf[0] = DS4_OUT_USB;
    fill_ds4_common(out, buf + 1);
    return DS4_OUT_USB_SIZE;
  }

  /* Over Bluetooth the pad ignores the whole report unless the first control
   * byte claims to be HID and to carry a CRC32. Sending 0x11 without them is
   * the usual reason a wired lightbar stops working once it goes wireless. */
  total = 1 + DS4_OUT_BT_SIZE;
  if (buflen < total) return 0;
  memset(buf, 0, total);
  buf[0] = DS_HID_HDR_OUTPUT;                        /* 0xA2, the CRC seed */
  buf[1] = DS4_OUT_BT;
  buf[2] = DS4_OUT_HWCTL_HID | DS4_OUT_HWCTL_CRC32;
  buf[3] = 0;                                         /* audio control */
  fill_ds4_common(out, buf + 4);

  crc = ds_crc32(0, buf, (uint32_t)(total - 4));
  buf[total - 4] = (uint8_t)(crc);
  buf[total - 3] = (uint8_t)(crc >> 8);
  buf[total - 2] = (uint8_t)(crc >> 16);
  buf[total - 1] = (uint8_t)(crc >> 24);
  return total;
}

bool ds_identify_bt_report(uint8_t report_id, ds_device_t *dev) {
  if (dev == NULL) return false;
  if (report_id == DS5_IN_BT_FULL) {          /* 0x31 */
    *dev = DS_DEVICE_DUALSENSE;
    return true;
  }
  if (report_id == DS4_IN_BT) {               /* 0x11 */
    *dev = DS_DEVICE_DUALSHOCK4;
    return true;
  }
  /* 0x01 is the report both pads fall back to, and it says nothing. */
  return false;
}

uint16_t ds_capabilities(ds_device_t dev) {
  if (dev == DS_DEVICE_DUALSHOCK4) {
    /* Its triggers are springs, there is no microphone and no lamps under the
     * touchpad. What it has instead is a light bar it blinks on its own. */
    return DS_CAP_LIGHTBAR_BLINK;
  }
  return DS_CAP_ADAPTIVE_TRIGGERS | DS_CAP_MIC_LED | DS_CAP_PLAYER_LEDS |
         DS_CAP_MUTE_BUTTON;
}

uint16_t ds_build_output_dev(const ds_output_t *out, ds_link_t link,
                              ds_device_t dev, uint8_t *buf, uint16_t buflen) {
  if (!out || !buf) return 0;
  if (dev == DS_DEVICE_DUALSHOCK4) return build_ds4_output(out, link, buf, buflen);
  return ds_build_output(out, link, buf, buflen);
}

uint16_t ds_build_output(const ds_output_t *out, ds_link_t link,
                          uint8_t *buf, uint16_t buflen) {
  if (!out || !buf) return 0;

  if (link == DS_LINK_USB) {
    if (buflen < DS5_OUT_USB_SIZE) return 0;
    memset(buf, 0, DS5_OUT_USB_SIZE);
    buf[0] = DS5_OUT_USB;
    fill_common(out, buf + 1);
    return DS5_OUT_USB_SIZE;
  }

  /* Bluetooth: 0xA2 transaction byte, report, CRC32 over both. */
  const uint16_t total = 1 + DS5_OUT_BT_SIZE;
  if (buflen < total) return 0;

  memset(buf, 0, total);
  buf[0] = DS_HID_HDR_OUTPUT;
  buf[1] = DS5_OUT_BT;
  buf[2] = (uint8_t)((out->sequence & 0x0F) << 4);   /* covered by the CRC */
  buf[3] = DS5_OUTPUT_TAG;
  fill_common(out, buf + 4);

  uint32_t crc = ds_crc32(0, buf, (uint32_t)(total - 4));
  buf[total - 4] = (uint8_t)(crc);
  buf[total - 3] = (uint8_t)(crc >> 8);
  buf[total - 2] = (uint8_t)(crc >> 16);
  buf[total - 1] = (uint8_t)(crc >> 24);
  return total;
}

/* ========================================================================== */
/*                                 I N P U T                                  */
/* ========================================================================== */

/* Offsets relative to the start of the report *body*, i.e. past the report ID
 * and, on Bluetooth's full report, past the sequence tag as well.
 *
 * The compatibility report happens to use the DualShock 4 field order, with
 * the buttons before the triggers. The full report is native DualSense order,
 * triggers first. Getting these two mixed up is the classic DualSense bug. */
typedef struct {
  int8_t lx, ly, rx, ry;
  int8_t btn0, btn1, btn2;
  int8_t l2, r2;
  int16_t status;     /* -1 when the report has none */
  int16_t gyro, accel, timestamp, touch;
  uint8_t body_off;
  uint8_t min_len;
  uint8_t ts_len;     /* sensor timestamp width: 4 here, 2 on a DualShock 4 */
  uint8_t motion_end; /* body offset one past the motion block              */
  bool has_mute;      /* a DualShock 4 keeps a report counter in those bits */
  bool cable_state;   /* ...and one cable bit where the charge code goes    */
} ds_layout_t;

static const ds_layout_t LAYOUT_COMPAT = {
  .lx = 0, .ly = 1, .rx = 2, .ry = 3,
  .btn0 = 4, .btn1 = 5, .btn2 = 6,
  .l2 = 7, .r2 = 8,
  .status = -1, .gyro = -1, .accel = -1, .timestamp = -1, .touch = -1,
  .body_off = 1, .min_len = DS5_IN_BT_COMPAT_SIZE,
  .ts_len = 0, .motion_end = 0, .has_mute = true, .cable_state = false,
};

/* Shared by BT report 0x31 and USB report 0x01 — only body_off differs. */
#define LAYOUT_FULL_FIELDS                                     \
  .lx = 0, .ly = 1, .rx = 2, .ry = 3,                          \
  .l2 = 4, .r2 = 5,                                            \
  .btn0 = 7, .btn1 = 8, .btn2 = 9,                             \
  .status = 52, .gyro = 15, .accel = 21,                       \
  .timestamp = 27, .ts_len = 4, .motion_end = 31, .touch = 32, \
  .has_mute = true, .cable_state = false

static const ds_layout_t LAYOUT_BT_FULL = {
  LAYOUT_FULL_FIELDS, .body_off = 2, .min_len = 13,
};

static const ds_layout_t LAYOUT_USB = {
  LAYOUT_FULL_FIELDS, .body_off = 1, .min_len = 12,
};

/* DualShock 4. The first nine body bytes are the compatibility layout above,
 * which is no coincidence — that report is a DualSense pretending to be this.
 * Everything after them is this controller's own:
 *
 *   9   sensor timestamp, two bytes, ahead of the sensors rather than behind
 *   11  die temperature, which nothing here reads
 *   12  gyro, three axes
 *   18  accelerometer, three axes
 *   29  status
 *   32  how many touch frames follow, then one frame: its own timestamp and
 *       then the same two four-byte points a DualSense uses
 *
 * Both links share every one of those; only how far in the body starts
 * differs, exactly as it does for the DualSense. */
#define LAYOUT_DS4_FIELDS                                      \
  .lx = 0, .ly = 1, .rx = 2, .ry = 3,                          \
  .btn0 = 4, .btn1 = 5, .btn2 = 6,                             \
  .l2 = 7, .r2 = 8,                                            \
  .timestamp = 9, .ts_len = 2, .motion_end = 24,               \
  .gyro = 12, .accel = 18,                                     \
  .status = 29, .touch = 34,                                   \
  .has_mute = false, .cable_state = true

static const ds_layout_t LAYOUT_DS4_USB = {
  LAYOUT_DS4_FIELDS, .body_off = 1, .min_len = 10,
};

/* Report 0x11 keeps two bytes of its own between the id and the body. */
static const ds_layout_t LAYOUT_DS4_BT = {
  LAYOUT_DS4_FIELDS, .body_off = 3, .min_len = 12,
};

/* Button bit masks, identical on every report. */
enum {
  BTN0_HAT = 0x0F,
  BTN0_SQUARE = 0x10, BTN0_CROSS = 0x20, BTN0_CIRCLE = 0x40, BTN0_TRIANGLE = 0x80,
  BTN1_L1 = 0x01, BTN1_R1 = 0x02, BTN1_L2 = 0x04, BTN1_R2 = 0x08,
  BTN1_CREATE = 0x10, BTN1_OPTIONS = 0x20, BTN1_L3 = 0x40, BTN1_R3 = 0x80,
  BTN2_PS = 0x01, BTN2_TOUCHPAD = 0x02, BTN2_MUTE = 0x04,
};

/* A DualShock 4 marks the cable with a single bit and then counts one step
 * further than a DualSense while it is in: 11 means charged. */
enum { DS4_ST0_CABLE = 0x10, DS4_BATTERY_FULL = 11 };

/* status[0]: low nibble battery, high nibble charge state.
 * status[1]: bit0 headphone, bit1 mic present, bit2 mic muted. */
enum {
  ST0_BATTERY = 0x0F, ST0_CHARGING = 0xF0,
  ST1_JACK = 0x03, ST1_MIC_MUTE = 0x04,
};

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

/* Hat index 0..7 walks clockwise from north; 8 means centred. */
static void decode_hat(uint8_t hat, ds_buttons_t *b) {
  static const uint8_t MAP[9] = {
    /* N  */ 0x1, /* NE */ 0x3, /* E  */ 0x2, /* SE */ 0x6,
    /* S  */ 0x4, /* SW */ 0xC, /* W  */ 0x8, /* NW */ 0x9,
    /* centre */ 0x0,
  };
  uint8_t m = MAP[hat > 8 ? 8 : hat];
  b->up = (m & 0x1) != 0;
  b->right = (m & 0x2) != 0;
  b->down = (m & 0x4) != 0;
  b->left = (m & 0x8) != 0;
}

static const ds_layout_t *pick_layout(const uint8_t *report, uint16_t len,
                                       ds_link_t link, ds_device_t dev) {
  if (dev == DS_DEVICE_DUALSHOCK4) {
    if (link == DS_LINK_USB) {
      if (report[0] == DS4_IN_USB && len >= LAYOUT_DS4_USB.min_len)
        return &LAYOUT_DS4_USB;
      return NULL;
    }
    /* The minimal Bluetooth report is the compatibility layout byte for
     * byte, so it needs no row of its own. */
    if (report[0] == DS4_IN_BT_MINIMAL && len >= LAYOUT_COMPAT.min_len)
      return &LAYOUT_COMPAT;
    if (report[0] == DS4_IN_BT && len >= LAYOUT_DS4_BT.min_len)
      return &LAYOUT_DS4_BT;
    return NULL;
  }
  if (link == DS_LINK_USB) {
    if (report[0] == DS5_IN_USB && len >= LAYOUT_USB.min_len) return &LAYOUT_USB;
    return NULL;
  }
  if (report[0] == DS5_IN_BT_COMPAT && len >= LAYOUT_COMPAT.min_len)
    return &LAYOUT_COMPAT;
  if (report[0] == DS5_IN_BT_FULL && len >= LAYOUT_BT_FULL.min_len)
    return &LAYOUT_BT_FULL;
  return NULL;
}

bool ds_parse_input(const uint8_t *report, uint16_t len, ds_link_t link,
                     ds_state_t *state) {
  return ds_parse_input_dev(report, len, link, DS_DEVICE_DUALSENSE, state);
}

bool ds_parse_input_dev(const uint8_t *report, uint16_t len, ds_link_t link,
                         ds_device_t dev, ds_state_t *state) {
  if (!report || !state || len < 2) return false;

  const ds_layout_t *l = pick_layout(report, len, link, dev);
  if (!l) return false;

  const uint8_t *b = report + l->body_off;
  uint16_t blen = len - l->body_off;

  memset(state, 0, sizeof(*state));
  state->report_id = report[0];

  state->analog.lx = b[l->lx];
  state->analog.ly = b[l->ly];
  state->analog.rx = b[l->rx];
  state->analog.ry = b[l->ry];
  state->analog.l2 = b[l->l2];
  state->analog.r2 = b[l->r2];

  uint8_t b0 = b[l->btn0], b1 = b[l->btn1], b2 = b[l->btn2];
  decode_hat(b0 & BTN0_HAT, &state->button);
  state->button.square = (b0 & BTN0_SQUARE) != 0;
  state->button.cross = (b0 & BTN0_CROSS) != 0;
  state->button.circle = (b0 & BTN0_CIRCLE) != 0;
  state->button.triangle = (b0 & BTN0_TRIANGLE) != 0;
  state->button.l1 = (b1 & BTN1_L1) != 0;
  state->button.r1 = (b1 & BTN1_R1) != 0;
  state->button.l2 = (b1 & BTN1_L2) != 0;
  state->button.r2 = (b1 & BTN1_R2) != 0;
  state->button.create = (b1 & BTN1_CREATE) != 0;
  state->button.options = (b1 & BTN1_OPTIONS) != 0;
  state->button.l3 = (b1 & BTN1_L3) != 0;
  state->button.r3 = (b1 & BTN1_R3) != 0;
  state->button.ps = (b2 & BTN2_PS) != 0;
  state->button.touchpad = (b2 & BTN2_TOUCHPAD) != 0;
  /* Not a button on every pad: a DualShock 4 counts reports in these bits,
   * and reading them as mute gives one that flickers on its own. */
  state->button.mute = l->has_mute && (b2 & BTN2_MUTE) != 0;

  /* motion_end rather than the timestamp offset plus four: a DualShock 4 puts
   * its timestamp in front of the sensors, so that sum stops short of the
   * accelerometer and would wave through a report truncated mid-motion. */
  if (l->gyro >= 0 && blen >= l->motion_end) {
    const uint8_t *g = b + l->gyro;
    const uint8_t *a = b + l->accel;
    const uint8_t *t = b + l->timestamp;
    for (int i = 0; i < 3; i++) {
      state->motion.gyro[i] = (int16_t)rd16(g + i * 2);
      state->motion.accel[i] = (int16_t)rd16(a + i * 2);
    }
    state->motion.timestamp = 0;
    for (int i = 0; i < (int)l->ts_len; i++)
      state->motion.timestamp |= (uint32_t)t[i] << (8 * i);
    state->has_motion = true;
  }

  /* Touchpad, 4 bytes per contact:
   *   [0] bit7 set while lifted, bits 0-6 = contact id
   *   [1] x low 8 bits
   *   [2] low nibble x high 4, high nibble y low 4
   *   [3] y high 8 bits */
  if (l->touch >= 0 && blen >= (uint16_t)l->touch + 8) {
    for (int i = 0; i < 2; i++) {
      const uint8_t *p = b + l->touch + i * 4;
      state->touch[i].active = (p[0] & 0x80) == 0;
      state->touch[i].id = p[0] & 0x7F;
      state->touch[i].x = (uint16_t)(p[1] | ((p[2] & 0x0F) << 8));
      state->touch[i].y = (uint16_t)(((p[2] & 0xF0) >> 4) | (p[3] << 4));
    }
    state->has_touch = true;
  }

  if (l->status >= 0 && blen >= (uint16_t)l->status + 2) {
    uint8_t s0 = b[l->status], s1 = b[l->status + 1];
    state->status.battery = s0 & ST0_BATTERY;
    if (l->cable_state) {
      /* One bit for the cable, and a capacity that runs to 11 while it is in.
       * Squashed into the same 0-10 and the same 1-charging / 2-full code the
       * DualSense reports, so a sketch does not have to ask which pad it has. */
      state->status.charging = (s0 & DS4_ST0_CABLE) ? 1 : 0;
      if (state->status.battery >= DS4_BATTERY_FULL) {
        state->status.battery = 10;
        if (s0 & DS4_ST0_CABLE) state->status.charging = 2;
      }
      /* A DualShock 4 reports neither a headphone jack nor a mic state here,
       * and there is no mic to mute. Both stay false. */
      (void)s1;
    } else {
      state->status.charging = (uint8_t)((s0 & ST0_CHARGING) >> 4);
      state->status.jack = (s1 & ST1_JACK) != 0;
      state->status.mic_muted = (s1 & ST1_MIC_MUTE) != 0;
    }
    state->status.valid = true;
  }

  return true;
}

const uint8_t *ds_find_bt_report(const uint8_t *payload, uint16_t payload_len,
                                  uint16_t *out_len) {
  if (!payload || payload_len < 3) return NULL;
  if (payload[0] != DS_HID_HDR_INPUT) return NULL;
  /* DS4_IN_BT_MINIMAL is the same 0x01 as the DualSense compatibility report,
   * so the three ids here cover both pads. Leaving 0x11 out was what stopped a
   * DualShock 4 ever reaching the parser over Bluetooth: the framing check
   * dropped every full report before anything got to look at it. */
  if (payload[1] != DS5_IN_BT_COMPAT && payload[1] != DS5_IN_BT_FULL &&
      payload[1] != DS4_IN_BT) {
    return NULL;
  }

  if (out_len) *out_len = (uint16_t)(payload_len - 1);
  return payload + 1;
}

/* ========================================================================== */
/*                       F E A T U R E   R E P O R T S                        */
/* ========================================================================== */

/* Field offsets of the calibration report, straight from
 * linux/drivers/hid/hid-playstation.c (dualsense_get_calibration_data). All
 * signed 16-bit little endian, starting one byte past the report id. */
enum {
  CAL_GYRO_PITCH_BIAS = 1,
  CAL_GYRO_YAW_BIAS = 3,
  CAL_GYRO_ROLL_BIAS = 5,
  CAL_GYRO_PITCH_PLUS = 7,
  CAL_GYRO_PITCH_MINUS = 9,
  CAL_GYRO_YAW_PLUS = 11,
  CAL_GYRO_YAW_MINUS = 13,
  CAL_GYRO_ROLL_PLUS = 15,
  CAL_GYRO_ROLL_MINUS = 17,
  CAL_GYRO_SPEED_PLUS = 19,
  CAL_GYRO_SPEED_MINUS = 21,
  CAL_ACC_X_PLUS = 23,
  CAL_ACC_X_MINUS = 25,
  CAL_ACC_Y_PLUS = 27,
  CAL_ACC_Y_MINUS = 29,
  CAL_ACC_Z_PLUS = 31,
  CAL_ACC_Z_MINUS = 33,
  CAL_MIN_LEN = 35
};

/* The units the rest of the library reports in: gyro / 1024 = deg/s and
 * accel / 8192 = g. Calibration exists to make those true. */
#define GYRO_RES_PER_DEG_S 1024
#define ACC_RES_PER_G      8192

static int16_t cal_i16(const uint8_t *p, int off) {
  return (int16_t)((uint16_t)p[off] | ((uint16_t)p[off + 1] << 8));
}

static void cal_passthrough(ds_axis_calib_t *a) {
  a->bias = 0;
  a->numer = 1;
  a->denom = 1;
}

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

/* A DualShock 4 lays its calibration out exactly as a DualSense does, with one
 * exception: over Bluetooth it groups the three gyro plus readings ahead of
 * the three minus ones instead of pairing them per axis. Reading a grouped
 * report as a paired one scales pitch by roll's numbers, quietly. */
static const int CAL_GYRO_PLUS_PAIRED[3] = {
  CAL_GYRO_PITCH_PLUS, CAL_GYRO_YAW_PLUS, CAL_GYRO_ROLL_PLUS
};
static const int CAL_GYRO_MINUS_PAIRED[3] = {
  CAL_GYRO_PITCH_MINUS, CAL_GYRO_YAW_MINUS, CAL_GYRO_ROLL_MINUS
};
static const int CAL_GYRO_PLUS_GROUPED[3] = {7, 9, 11};
static const int CAL_GYRO_MINUS_GROUPED[3] = {13, 15, 17};

static bool parse_calibration(const uint8_t *report, uint16_t len,
                              uint8_t want_id, const int *gyro_plus,
                              const int *gyro_minus, ds_calibration_t *cal) {
  int32_t speed_2x, range_2g;
  int i;

  if (report == NULL || cal == NULL) return false;
  for (i = 0; i < 3; i++) {
    cal_passthrough(&cal->gyro[i]);
    cal_passthrough(&cal->accel[i]);
  }
  cal->valid = false;
  if (len < CAL_MIN_LEN || report[0] != want_id) return false;

  /* Gyro: sensitivity only. The plus and minus figures are the readings taken
   * while the axis was turned at a known rate, and their span against the
   * reference bias gives the scale. */
  speed_2x = (int32_t)cal_i16(report, CAL_GYRO_SPEED_PLUS) +
             (int32_t)cal_i16(report, CAL_GYRO_SPEED_MINUS);
  {
    const int *plus = gyro_plus;
    const int *minus = gyro_minus;
    const int bias[3] = {CAL_GYRO_PITCH_BIAS, CAL_GYRO_YAW_BIAS,
                         CAL_GYRO_ROLL_BIAS};
    for (i = 0; i < 3; i++) {
      int32_t b = cal_i16(report, bias[i]);
      int32_t denom = abs32((int32_t)cal_i16(report, plus[i]) - b) +
                      abs32((int32_t)cal_i16(report, minus[i]) - b);
      if (denom == 0 || speed_2x == 0) continue;   /* leave pass-through */
      cal->gyro[i].bias = 0;
      cal->gyro[i].numer = speed_2x * GYRO_RES_PER_DEG_S;
      cal->gyro[i].denom = denom;
    }
  }

  /* Accelerometer: the plus and minus readings are 1 g either way, so their
   * midpoint is the zero and their span is 2 g. */
  {
    const int plus[3] = {CAL_ACC_X_PLUS, CAL_ACC_Y_PLUS, CAL_ACC_Z_PLUS};
    const int minus[3] = {CAL_ACC_X_MINUS, CAL_ACC_Y_MINUS, CAL_ACC_Z_MINUS};
    for (i = 0; i < 3; i++) {
      int32_t p = cal_i16(report, plus[i]);
      range_2g = p - (int32_t)cal_i16(report, minus[i]);
      if (range_2g == 0) continue;
      cal->accel[i].bias = (int16_t)(p - range_2g / 2);
      cal->accel[i].numer = 2 * ACC_RES_PER_G;
      cal->accel[i].denom = range_2g;
    }
  }

  cal->valid = true;
  return true;
}

bool ds_parse_calibration(const uint8_t *report, uint16_t len,
                           ds_calibration_t *cal) {
  return parse_calibration(report, len, DS5_FEATURE_CALIBRATION,
                           CAL_GYRO_PLUS_PAIRED, CAL_GYRO_MINUS_PAIRED, cal);
}

bool ds_parse_calibration_dev(const uint8_t *report, uint16_t len,
                               ds_link_t link, ds_device_t dev,
                               ds_calibration_t *cal) {
  if (dev != DS_DEVICE_DUALSHOCK4)
    return ds_parse_calibration(report, len, cal);
  if (link == DS_LINK_USB)
    return parse_calibration(report, len, DS4_FEATURE_CALIBRATION,
                             CAL_GYRO_PLUS_PAIRED, CAL_GYRO_MINUS_PAIRED, cal);
  return parse_calibration(report, len, DS4_FEATURE_CALIBRATION_BT,
                           CAL_GYRO_PLUS_GROUPED, CAL_GYRO_MINUS_GROUPED, cal);
}

/* The gyro correction is a change of unit by a factor of about sixty, so it
 * has to be worked out in 64 bits: a reading near the end of its range times a
 * numerator near a million overflows int32 on the way through. */
static int32_t apply_gyro(const ds_axis_calib_t *a, int32_t raw) {
  int64_t v;
  if (a->denom == 0) return raw;
  v = (int64_t)(raw - a->bias) * a->numer / a->denom;
  if (v > 2147483647LL) return 2147483647L;
  if (v < -2147483648LL) return (-2147483647L - 1);
  return (int32_t)v;
}

/* The accelerometer correction is a percent or two and stays in 16 bits. */
static int16_t apply_accel(const ds_axis_calib_t *a, int16_t raw) {
  int32_t v;
  if (a->denom == 0) return raw;
  v = ((int32_t)raw - a->bias) * a->numer / a->denom;
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

void ds_apply_calibration(const ds_calibration_t *cal,
                           ds_motion_t *motion) {
  int i;
  if (cal == NULL || motion == NULL || !cal->valid) return;
  for (i = 0; i < 3; i++) {
    motion->gyro[i] = apply_gyro(&cal->gyro[i], motion->gyro[i]);
    motion->accel[i] = apply_accel(&cal->accel[i], motion->accel[i]);
  }
}

bool ds_parse_pairing_info(const uint8_t *report, uint16_t len,
                            uint8_t addr[6]) {
  if (report == NULL || addr == NULL) return false;
  if (len < 7 || report[0] != DS5_FEATURE_PAIRING_INFO) return false;
  memcpy(addr, report + 1, 6);
  return true;
}

bool ds_parse_pairing_info_dev(const uint8_t *report, uint16_t len,
                                ds_device_t dev, uint8_t addr[6]) {
  if (dev != DS_DEVICE_DUALSHOCK4)
    return ds_parse_pairing_info(report, len, addr);
  if (report == NULL || addr == NULL) return false;
  /* Same six bytes in the same place, behind a different report id. */
  if (len < 7 || report[0] != DS4_FEATURE_PAIRING_INFO) return false;
  memcpy(addr, report + 1, 6);
  return true;
}

uint16_t ds_build_bt_feature_request(uint8_t report_id, uint8_t *buf,
                                      uint16_t buflen) {
  if (buf == NULL || buflen < 2) return 0;
  buf[0] = DS_HID_HDR_GET_FEATURE;
  buf[1] = report_id;
  return 2;
}

const uint8_t *ds_find_bt_feature(const uint8_t *payload, uint16_t payload_len,
                                   uint16_t *out_len) {
  uint32_t want, got;
  uint16_t body;

  /* transaction byte + report id + four bytes of CRC is the shortest useful
   * answer, and even that carries no data. */
  if (payload == NULL || payload_len < 6) return NULL;
  if (payload[0] != DS_HID_HDR_FEATURE) return NULL;

  body = (uint16_t)(payload_len - 4);
  want = (uint32_t)payload[body] | ((uint32_t)payload[body + 1] << 8) |
         ((uint32_t)payload[body + 2] << 16) |
         ((uint32_t)payload[body + 3] << 24);
  /* The signature covers the transaction byte as well as the report, which is
   * why the seed and the first byte are the same thing. */
  got = ds_crc32(0, payload, body);
  if (got != want) return NULL;

  if (out_len) *out_len = (uint16_t)(body - 1);
  return payload + 1;
}

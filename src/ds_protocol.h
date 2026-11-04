/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_protocol.h — DualSense wire protocol, independent of any transport.
 *
 * This layer knows how a DualSense encodes its reports and nothing else: no
 * Bluetooth, no USB, no ESP-IDF, no Arduino. Feed it a report and it fills in
 * a state struct; ask it for an output report and it hands back the bytes to
 * put on the wire. That makes the same code usable from
 *
 *   - Bluetooth Classic on the original ESP32 (Bluedroid L2CAP)
 *   - a USB Bluetooth dongle driven over USB host (ESP32-S3)
 *   - a DualSense plugged straight into a USB host
 *
 * Sources for every offset and constant in here:
 *   linux/drivers/hid/hid-playstation.c   (report structs, CRC seeds)
 *   https://github.com/nondebug/dualsense (BT compatibility report 0x01)
 *   https://github.com/HamzaYslmn/esp-ps5 (independent wire-offset write-up)
 *   https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db
 *                                         (adaptive trigger bit packing)
 * Verified against real hardware; see VERIFICATION.md.
 */
#ifndef DS_PROTOCOL_H
#define DS_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*                              L I N K   T Y P E                             */
/* ========================================================================== */

/* Which physical link the reports are travelling over. The DualSense uses
 * different report IDs, different field order and different framing on each,
 * so almost every call needs to know. */
typedef enum {
  DS_LINK_BT = 0,   /* Bluetooth Classic: input 0x31 / output 0x31 + CRC32 */
  DS_LINK_USB = 1,  /* USB HID:           input 0x01 / output 0x02, no CRC */
} ds_link_t;

/* ========================================================================== */
/*                             D E V I C E   T Y P E                          */
/* ========================================================================== */

/* Which controller is on the other end.
 *
 * The DualShock 4 is near enough to share every layer of this library and far
 * enough to need its own tables. The framing is identical — 0xA1 in, 0xA2 out,
 * the same CRC32 over Bluetooth, the same four-byte touch point packing — and
 * so are the first nine bytes of the report body, sticks then buttons then
 * triggers. Past that it diverges:
 *
 *   - a two-byte sensor timestamp *before* the gyro, where the DualSense puts
 *     a four-byte one after the accelerometer
 *   - no mute button; those bits carry a report counter, so decoding them as
 *     a button gives a mute that flickers on its own
 *   - a single cable-state bit where the DualSense keeps a four-bit charge code
 *   - an output report with a different id, a different length, no adaptive
 *     triggers, no mic LED, no player LEDs, and a lightbar that can blink by
 *     itself
 *
 * Over USB both pads use input report 0x01 at 64 bytes, so a report on its own
 * cannot tell you which one is talking. The link knows, from the USB product
 * id, and passes it down. */
typedef enum {
  DS_DEVICE_DUALSENSE = 0,
  DS_DEVICE_DUALSHOCK4 = 1,
} ds_device_t;

/* What the hardware on the other end can actually do.
 *
 * Ask this rather than testing the device, so a sketch says what it needs
 * instead of what it has heard of. The light bar, rumble, the touchpad, motion
 * and the battery are on every pad and need no flag; these are only the parts
 * that come and go.
 *
 * The point of having it at all: a call the pad cannot honour is dropped when
 * the report is built, and a report that is one field short looks exactly like
 * a report that arrived. Silence is the wrong answer, so the C++ wrapper turns
 * these into a refusal you can see. */
typedef enum {
  DS_CAP_ADAPTIVE_TRIGGERS = 1u << 0,  /* L2/R2 push back            */
  DS_CAP_MIC_LED           = 1u << 1,  /* the lamp on the mute key   */
  DS_CAP_PLAYER_LEDS       = 1u << 2,  /* five lamps under the pad   */
  DS_CAP_MUTE_BUTTON       = 1u << 3,  /* there is a mic to mute     */
  DS_CAP_LIGHTBAR_BLINK    = 1u << 4,  /* the pad blinks it itself   */
} ds_cap_t;

/* The set for one controller. Unknown devices get the DualSense set, which is
 * what an unrecognised pad is treated as everywhere else. */
uint16_t ds_capabilities(ds_device_t dev);

/* Which controller sends this Bluetooth input report id.
 *
 * There is no product id over Bluetooth, so the report itself is the only
 * thing that names the pad: 0x31 is a DualSense and 0x11 a DualShock 4.
 * Returns false for 0x01, which both pads send until a feature report is
 * asked for and which therefore identifies nothing — the layout is the same
 * either way, so parsing it as the wrong pad still decodes correctly. */
bool ds_identify_bt_report(uint8_t report_id, ds_device_t *dev);

/* ========================================================================== */
/*                            R E P O R T   S I Z E S                         */
/* ========================================================================== */

#define DS5_IN_BT_COMPAT       0x01  /* 10 bytes, sticks + buttons only      */
#define DS5_IN_BT_COMPAT_SIZE  10
#define DS5_IN_BT_FULL         0x31  /* 78 bytes, everything                 */
#define DS5_IN_BT_FULL_SIZE    78
#define DS5_IN_USB             0x01  /* 64 bytes, everything                 */
#define DS5_IN_USB_SIZE        64

#define DS5_OUT_BT             0x31
#define DS5_OUT_BT_SIZE        78    /* + a 0xA2 transaction byte on the wire */
#define DS5_OUT_USB            0x02
#define DS5_OUT_USB_SIZE       63

/* DualShock 4. Its 10-byte Bluetooth report is byte for byte the DualSense
 * compatibility report above — that is where the layout came from. */
#define DS4_IN_USB             0x01  /* 64 bytes, everything                 */
#define DS4_IN_USB_SIZE        64
#define DS4_IN_BT_MINIMAL      0x01  /* 10 bytes, sticks + buttons only      */
#define DS4_IN_BT_MINIMAL_SIZE 10
#define DS4_IN_BT              0x11  /* 78 bytes, everything                 */
#define DS4_IN_BT_SIZE         78

#define DS4_OUT_USB            0x05
#define DS4_OUT_USB_SIZE       32
#define DS4_OUT_BT             0x11
#define DS4_OUT_BT_SIZE        78    /* + a 0xA2 transaction byte on the wire */

/* The Bluetooth output report only reaches the motors and the lightbar once
 * bit 7 says "this is HID" and bit 6 says "there is a CRC32 on the end". The
 * low six bits are a poll interval the pad may honour; zero is fine. */
#define DS4_OUT_HWCTL_HID      0x80
#define DS4_OUT_HWCTL_CRC32    0x40

/* Touchpad extent, for anyone scaling to a screen. Same width, and the
 * DualShock 4 is the shorter of the two. */
#define DS5_TOUCH_WIDTH        1920
#define DS5_TOUCH_HEIGHT       1080
#define DS4_TOUCH_WIDTH        1920
#define DS4_TOUCH_HEIGHT        942

/* Bluetooth HID transaction headers. The output one doubles as the CRC seed:
 * the controller signs the transaction byte along with the report. */
#define DS_HID_HDR_INPUT      0xA1
#define DS_HID_HDR_OUTPUT     0xA2

/* Largest buffer any build call can need, including the transaction byte. */
#define DS_OUT_MAX_SIZE       (1 + DS5_OUT_BT_SIZE)

/* ========================================================================== */
/*                                 S T A T E                                  */
/* ========================================================================== */

typedef struct {
  uint8_t lx, ly, rx, ry;   /* raw 0-255, 128 = centre */
  uint8_t l2, r2;           /* trigger pressure 0-255  */
} ds_analog_t;

typedef struct {
  bool up, right, down, left;
  bool square, cross, circle, triangle;
  bool l1, r1, l2, r2;      /* l2/r2 here are the digital click */
  bool create, options, l3, r3;
  bool ps, touchpad, mute;
} ds_buttons_t;

/* The two sensors do NOT arrive in the same kind of units, which is a good way
 * to get motion code badly wrong:
 *
 *   accel  8192 LSB per g as it comes off the wire. Feature report 0x05 only
 *          trims that by a percent or two.
 *   gyro   roughly 16 LSB per deg/s as it comes off the wire, and the exact
 *          figure differs from controller to controller. Feature report 0x05
 *          holds the real one. After ds_apply_calibration the axis reads
 *          exactly 1024 LSB per deg/s.
 *
 * That last conversion is a factor of about sixty, which is why gyro is 32
 * bits: at 1024 LSB per deg/s a 16-bit field runs out at 32 deg/s, and a
 * flick of the wrist pins all three axes at once. */
typedef struct {
  int32_t gyro[3];    /* pitch, yaw, roll */
  int16_t accel[3];   /* x, y, z          */
  uint32_t timestamp; /* controller clock, 0.33 us units */
} ds_motion_t;

typedef struct {
  bool active;
  uint8_t id;         /* bumped on each new contact, so a drag keeps its id */
  uint16_t x;         /* 0-1919 */
  uint16_t y;         /* 0-1079 */
} ds_touch_point_t;

typedef struct {
  uint8_t battery;    /* 0-10, one unit is roughly 10%  */
  uint8_t charging;   /* raw 4-bit code; 1 = charging, 2 = full */
  bool jack;          /* headphone or mic plugged in */
  bool mic_muted;
  bool valid;         /* false when the report carries no status at all */
} ds_status_t;

typedef struct {
  uint8_t report_id;  /* which report this state was decoded from */
  ds_analog_t analog;
  ds_buttons_t button;
  ds_motion_t motion;
  ds_touch_point_t touch[2];
  ds_status_t status;
  bool has_motion;    /* the compatibility report carries neither */
  bool has_touch;
} ds_state_t;

/* ========================================================================== */
/*                        A D A P T I V E   T R I G G E R S                   */
/* ========================================================================== */

/* An effect is an 11-byte block: one mode byte plus ten parameters.
 * Only L2 and R2 have an actuator; L1/R1 are plain digital bumpers. */
#define DS5_TRIGGER_BLOCK_SIZE 11

typedef enum {
  DS5_TRIGGER_RIGHT = 0,  /* R2 */
  DS5_TRIGGER_LEFT = 1,   /* L2 */
} ds5_trigger_t;

/* Positions below are the controller's ten zones: 0 = released, 9 = bottomed
 * out. Every builder returns false and writes the Off effect when the
 * parameters are out of range, matching Sony's own behaviour. */

void ds5_trigger_off(uint8_t block[DS5_TRIGGER_BLOCK_SIZE]);

/* Constant resistance from `position` (0-9) onwards. strength 0-8. */
bool ds5_trigger_feedback(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                          uint8_t position, uint8_t strength);

/* "Trigger lock": resists from `start` (2-7) to `end` (start+1..8) then gives
 * way, the way a gun trigger breaks. strength 0-8. */
bool ds5_trigger_weapon(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                        uint8_t start, uint8_t end, uint8_t strength);

/* Buzzes beyond `position` (0-9) at `amplitude` (0-8) and `frequency` Hz. */
bool ds5_trigger_vibration(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                           uint8_t position, uint8_t amplitude,
                           uint8_t frequency);

/* --- the general forms ----------------------------------------------------
 *
 * Feedback and Vibration above are these with every zone set the same. Ten
 * entries, one per zone, each 0-8; zero leaves that zone alone entirely.
 */
bool ds5_trigger_multi_feedback(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                                const uint8_t strength[10]);
bool ds5_trigger_multi_vibration(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                                 uint8_t frequency,
                                 const uint8_t amplitude[10]);

/* Resistance ramping from one strength to another between two positions, and
 * holding the second one to the end of the travel. start 0-8, end above it
 * and up to 9, both strengths 1-8. */
bool ds5_trigger_slope(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t start,
                       uint8_t end, uint8_t start_strength,
                       uint8_t end_strength);

/* --- and the three Sony does not document ---------------------------------
 *
 * Worked out by the community rather than published, so treat the parameter
 * ranges as what the controller was observed to accept.
 */

/* Rising tension that lets go at the end, like drawing a bow. Positions 0-8
 * with end above start, strength and snap_force 1-8. */
bool ds5_trigger_bow(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t start,
                     uint8_t end, uint8_t strength, uint8_t snap_force);

/* Two beats repeating, a horse rather than a buzz. start 0-8, end above it up
 * to 9, first_foot 0-6, second_foot above it up to 7, frequency in Hz. */
bool ds5_trigger_galloping(uint8_t block[DS5_TRIGGER_BLOCK_SIZE],
                           uint8_t start, uint8_t end, uint8_t first_foot,
                           uint8_t second_foot, uint8_t frequency);

/* Two alternating amplitudes: an engine, or a saw. start 0-8, end above it up
 * to 9, both amplitudes 0-7, frequency in Hz, period sets how long it stays
 * on each. */
bool ds5_trigger_machine(uint8_t block[DS5_TRIGGER_BLOCK_SIZE], uint8_t start,
                         uint8_t end, uint8_t amplitude_a, uint8_t amplitude_b,
                         uint8_t frequency, uint8_t period);

/* ========================================================================== */
/*                                O U T P U T                                 */
/* ========================================================================== */

/* Everything the host can drive. Fields are only applied if the matching
 * `set_*` flag is true, so a caller can update the lightbar without disturbing
 * a trigger effect it did not set this frame. */
typedef struct {
  bool set_rumble;
  uint8_t rumble_left;    /* low frequency  / "large"  */
  uint8_t rumble_right;   /* high frequency / "small"  */

  bool set_lightbar;
  uint8_t red, green, blue;

  bool set_player_leds;
  uint8_t player_leds;    /* low 5 bits, one per lamp */
  uint8_t led_brightness; /* 0-2 */

  bool set_mic_led;
  uint8_t mic_led;        /* 0 off, 1 solid, 2 pulse */

  bool set_trigger_left;
  uint8_t trigger_left[DS5_TRIGGER_BLOCK_SIZE];

  bool set_trigger_right;
  uint8_t trigger_right[DS5_TRIGGER_BLOCK_SIZE];

  /* DualShock 4 only. Its lightbar blinks on its own once these are set, in
   * units of 10 ms; both zero means solid. The DualSense has no equivalent
   * and ignores them, so leaving the flag set costs nothing there. */
  bool set_lightbar_blink;
  uint8_t blink_on, blink_off;

  /* Bluetooth only. The controller expects the low 4 bits to advance with each
   * frame. It must be set before the report is built, not stamped afterwards:
   * the CRC32 covers this byte. Ignored on USB. */
  uint8_t sequence;
} ds_output_t;

/* Zero the struct and load the neutral defaults. */
void ds_output_init(ds_output_t *out);

/* Serialises `out` into `buf`.
 *
 * For DS_LINK_BT the result is 79 bytes and already carries the leading 0xA2
 * transaction byte and the trailing CRC32 — hand it straight to the interrupt
 * channel. For DS_LINK_USB it is 63 bytes starting at report ID 0x02.
 *
 * Returns the number of bytes written, or 0 if `buf` is too small. */
uint16_t ds_build_output(const ds_output_t *out, ds_link_t link,
                          uint8_t *buf, uint16_t buflen);

/* The same, for a named controller. ds_build_output() is this with
 * DS_DEVICE_DUALSENSE.
 *
 * For a DualShock 4 the result is 32 bytes from report id 0x05 on USB, or 79
 * bytes over Bluetooth: the 0xA2 transaction byte, report id 0x11, the two
 * control bytes and a trailing CRC32. Only rumble, the lightbar and its blink
 * survive the trip — a DualShock 4 has no adaptive triggers, no mic LED and
 * no player LEDs, and those fields are dropped rather than misplaced. */
uint16_t ds_build_output_dev(const ds_output_t *out, ds_link_t link,
                              ds_device_t dev, uint8_t *buf, uint16_t buflen);

/* ========================================================================== */
/*                                 I N P U T                                  */
/* ========================================================================== */

/* Decodes one input report.
 *
 * `report` must point at the HID report ID, i.e. past any 0xA1 transaction
 * byte. Unknown report IDs and short reports are rejected rather than decoded
 * as if they were something else.
 *
 * `state` is only written on success; on failure it is left untouched so a
 * caller can keep showing the last good frame. */
bool ds_parse_input(const uint8_t *report, uint16_t len, ds_link_t link,
                     ds_state_t *state);

/* The same, for a named controller. ds_parse_input() is this with
 * DS_DEVICE_DUALSENSE.
 *
 * Passing the wrong device is not caught: over USB both pads answer to report
 * id 0x01 at 64 bytes, and the sticks, buttons and triggers land in the same
 * place either way, so the mistake shows up as motion and touch that decode
 * to plausible nonsense rather than as a failure. */
bool ds_parse_input_dev(const uint8_t *report, uint16_t len, ds_link_t link,
                         ds_device_t dev, ds_state_t *state);

/* Locates the report inside a raw Bluetooth L2CAP payload by validating the
 * 0xA1 transaction byte and the report ID. Returns NULL when the payload is
 * not an input report we understand (HID handshakes, feature responses...).
 * `out_len` receives the length from the report ID onwards. */
const uint8_t *ds_find_bt_report(const uint8_t *payload, uint16_t payload_len,
                                  uint16_t *out_len);

/* ========================================================================== */
/*                       F E A T U R E   R E P O R T S                        */
/* ========================================================================== */

/* Feature reports are fetched over the control pipe on USB, or with a HID
 * GET_REPORT on the control channel over Bluetooth. This layer only decodes
 * them; asking for them is the transport's job. */

#define DS5_FEATURE_CALIBRATION       0x05
#define DS5_FEATURE_CALIBRATION_SIZE  41
#define DS5_FEATURE_PAIRING_INFO      0x09
#define DS5_FEATURE_PAIRING_INFO_SIZE 20

/* The DualShock 4 keeps its calibration behind a different id on each link,
 * and the Bluetooth one is four bytes longer. */
#define DS4_FEATURE_CALIBRATION          0x02
#define DS4_FEATURE_CALIBRATION_SIZE     37
#define DS4_FEATURE_CALIBRATION_BT       0x05
#define DS4_FEATURE_CALIBRATION_BT_SIZE  41
#define DS4_FEATURE_PAIRING_INFO         0x12
#define DS4_FEATURE_PAIRING_INFO_SIZE    16

/* Over Bluetooth a feature report is asked for on the HID control channel and
 * comes back on it. Two bytes go out — the transaction byte and the report id
 * — and the answer is signed the same way output reports are, with its own
 * transaction byte as the CRC seed. */
#define DS_HID_HDR_GET_FEATURE 0x43   /* host to pad: GET_REPORT, feature */
#define DS_HID_HDR_FEATURE     0xA3   /* pad to host, and the CRC seed    */

/* Builds the two byte request. `buf` needs 2 bytes; returns the length. */
uint16_t ds_build_bt_feature_request(uint8_t report_id, uint8_t *buf,
                                      uint16_t buflen);

/* The counterpart to ds_find_bt_report, for the control channel: checks the
 * 0xA3 transaction byte and the trailing CRC32, and returns the report
 * starting at its id. NULL for anything else, including a bad CRC — a feature
 * report that fails its checksum is worse than none, because calibration
 * silently scales every reading afterwards. */
const uint8_t *ds_find_bt_feature(const uint8_t *payload, uint16_t payload_len,
                                   uint16_t *out_len);

/* Normalisation for one axis: out = (raw - bias) * numer / denom. */
typedef struct {
  int16_t bias;
  int32_t numer;
  int32_t denom;
} ds_axis_calib_t;

typedef struct {
  ds_axis_calib_t gyro[3];    /* pitch, yaw, roll */
  ds_axis_calib_t accel[3];   /* x, y, z          */
  bool valid;
} ds_calibration_t;

/* Decodes the factory calibration. An axis whose numbers are unusable — a
 * clone, or a report that never arrived — is left as a pass-through rather
 * than being allowed to scale readings into nonsense.
 *
 * Note that the gyro comes out with a zero bias. The DualSense has already
 * removed its own, and the bias fields in the report describe the reference
 * point the sensitivity figures were measured against, not an offset to
 * subtract. Treating them as an offset is a popular way to make a resting
 * controller drift. */
bool ds_parse_calibration(const uint8_t *report, uint16_t len,
                           ds_calibration_t *cal);

/* The same, for a named controller. ds_parse_calibration() is this with
 * DS_DEVICE_DUALSENSE, whose report is the same over either link.
 *
 * A DualShock 4 needs `link` as well, because it reorders the six gyro
 * sensitivity fields between the two: over USB they alternate plus and minus
 * per axis, over Bluetooth all three plus values come first. Getting that
 * backwards scales pitch by roll's numbers. */
bool ds_parse_calibration_dev(const uint8_t *report, uint16_t len,
                               ds_link_t link, ds_device_t dev,
                               ds_calibration_t *cal);

/* Rewrites `motion` in place. Safe to call with an invalid calibration, in
 * which case it does nothing. */
void ds_apply_calibration(const ds_calibration_t *cal, ds_motion_t *motion);

/* The controller's own Bluetooth address, in HCI wire order — the same order
 * ds_hci_addr_to_str and ds_hci_cmd_create_connection want, so a pad read
 * over USB can be handed straight to the Bluetooth side. */
bool ds_parse_pairing_info(const uint8_t *report, uint16_t len,
                            uint8_t addr[6]);

/* The same, for a named controller. Identical six bytes in an identically
 * shaped report; only the id it hides behind differs. */
bool ds_parse_pairing_info_dev(const uint8_t *report, uint16_t len,
                                ds_device_t dev, uint8_t addr[6]);

/* ========================================================================== */
/*                                  C R C 3 2                                 */
/* ========================================================================== */

/* Standard CRC-32 (poly 0xEDB88320, init/final inverted) — the one Bluetooth
 * DualSense reports are signed with. Exposed because a transport may already
 * have a hardware-accelerated version it would rather use; define
 * DS_CRC32_EXTERNAL and provide this symbol yourself to override it. */
uint32_t ds_crc32(uint32_t crc, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* DS_PROTOCOL_H */

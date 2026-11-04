# -*- coding: utf-8 -*-
"""Generate ds4_vectors.h for the DualShock 4 self-test.

Every byte here is laid out from the report structs in Linux's
drivers/hid/hid-playstation.c and the CRC comes from zlib, not from
ds_crc32 — so a transcription slip in the library fails a test instead of
being baked into both sides.

  struct dualshock4_input_report_common {
      u8 x, y, rx, ry;          buttons[3];  z, rz;
      __le16 sensor_timestamp;  u8 sensor_temperature;
      __le16 gyro[3];           __le16 accel[3];
      u8 reserved2[5];          u8 status[2];   u8 reserved3;
  }                                                  -- 32 bytes
  usb: report_id, common, num_touch_reports, touch_reports[3], reserved[3]
  bt:  report_id, reserved[2], common, num_touch_reports,
       touch_reports[4], reserved2[2], crc32

  struct dualshock4_output_report_common {
      u8 valid_flag0, valid_flag1, reserved;
      u8 motor_right, motor_left;
      u8 lightbar_red, lightbar_green, lightbar_blue;
      u8 lightbar_blink_on, lightbar_blink_off;
  }                                                  -- 10 bytes
"""
import io
import os
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ds4_vectors.h")


def le16(v):
    return [v & 0xFF, (v >> 8) & 0xFF]


def s16(v):
    return le16(v & 0xFFFF)


# --------------------------------------------------------------- the common body
# Chosen so every field is distinguishable from every other one.
body = [0] * 63
body[0:4] = [0x10, 0x20, 0x30, 0x40]        # lx ly rx ry
# hat 2 = east, and square + circle down
body[4] = 0x02 | 0x10 | 0x40                # 0x52
# l1, l2 click, share, r3
body[5] = 0x01 | 0x04 | 0x10 | 0x80         # 0x95
# PS down, and a report counter in the bits a DualSense uses for mute
body[6] = 0x01 | 0x3C                       # 0x3D
body[7] = 0x7F                              # l2 analog
body[8] = 0xC0                              # r2 analog
body[9:11] = le16(0x1234)                   # sensor timestamp, 2 bytes
body[11] = 0x19                             # die temperature
body[12:18] = s16(100) + s16(-200) + s16(300)         # gyro pitch yaw roll
body[18:24] = s16(1000) + s16(-2000) + s16(8192)      # accel x y z
body[29] = 0x07 | 0x10                      # battery 7, cable in
body[30] = 0x00
body[32] = 1                                # one touch frame follows
body[33] = 0x55                             # that frame's own timestamp
body[34:38] = [0x02, 0x23, 0x61, 0x45]      # contact 2, x 0x123, y 0x456
body[38:42] = [0x80, 0x00, 0x00, 0x00]      # second finger lifted

# A second body that differs only in the battery byte: 11 with the cable in is
# how a DualShock 4 says "charged", a value a DualSense never sends.
body_full = list(body)
body_full[29] = 11 | 0x10

# ------------------------------------------------------------------- USB input
usb_in = [0x01] + body
assert len(usb_in) == 64, len(usb_in)

usb_in_full = [0x01] + body_full

# -------------------------------------------------------------------- BT input
# report id, two bytes of its own, then the identical body.
bt_in = [0x11, 0x00, 0x00] + body[:32]
bt_in += [body[32], body[33]] + body[34:42]        # num_touch + one frame
bt_in += [0] * (78 - len(bt_in) - 4)               # remaining frames + reserved
bt_in += list(zlib.crc32(bytes([0xA1] + bt_in)).to_bytes(4, "little"))
assert len(bt_in) == 78, len(bt_in)

# ------------------------------------------------------------------ USB output
out_common = [
    0x07,        # valid_flag0: motor | led | led_blink
    0x00,        # valid_flag1 is audio only
    0x00,        # reserved
    0x80,        # motor_right, the small high frequency one
    0x40,        # motor_left, the large low frequency one
    0x10, 0x20, 0x30,   # lightbar r g b
    0x05, 0x06,  # blink on / off, in units of 10 ms
]
usb_out = [0x05] + out_common + [0] * 21
assert len(usb_out) == 32, len(usb_out)

# ------------------------------------------------------------------- BT output
bt_out = [0xA2, 0x11, 0xC0, 0x00] + out_common     # 0xC0 = HID | CRC32
bt_out += [0] * (79 - len(bt_out) - 4)
bt_out += list(zlib.crc32(bytes(bt_out)).to_bytes(4, "little"))
assert len(bt_out) == 79, len(bt_out)

# ------------------------------------------------------------- calibration
# 37 bytes. Gyro sensitivity is deliberately different per axis so a wrong
# plus/minus ordering cannot accidentally produce the right scale.
def calib(report_id, grouped):
    c = [0] * 37
    c[0] = report_id
    c[1:7] = s16(10) + s16(20) + s16(30)            # pitch/yaw/roll bias
    plus = {"pitch": 4000, "yaw": 5000, "roll": 6000}
    minus = {"pitch": -4000, "yaw": -5000, "roll": -6000}
    if grouped:                                      # Bluetooth: all plus first
        c[7:19] = (s16(plus["pitch"]) + s16(plus["yaw"]) + s16(plus["roll"]) +
                   s16(minus["pitch"]) + s16(minus["yaw"]) + s16(minus["roll"]))
    else:                                            # USB: paired per axis
        c[7:19] = (s16(plus["pitch"]) + s16(minus["pitch"]) +
                   s16(plus["yaw"]) + s16(minus["yaw"]) +
                   s16(plus["roll"]) + s16(minus["roll"]))
    c[19:23] = s16(120) + s16(120)                   # gyro speed plus / minus
    c[23:35] = (s16(8000) + s16(-8000) + s16(8100) + s16(-8100) +
                s16(8200) + s16(-8200))              # accel x/y/z plus, minus
    return c


cal_usb = calib(0x02, grouped=False)
cal_bt = calib(0x05, grouped=True)

# What the library should work out from those numbers, per axis:
#   numer = (speed_plus + speed_minus) * 1024,  denom = |plus-bias| + |minus-bias|
speed_2x = 120 + 120
expect = []
for bias, p, m in ((10, 4000, -4000), (20, 5000, -5000), (30, 6000, -6000)):
    expect.append((speed_2x * 1024, abs(p - bias) + abs(m - bias)))


def emit(f, name, data):
    f.write("static const uint8_t %s[] = {\n" % name)
    for i in range(0, len(data), 12):
        f.write("  " + " ".join("0x%02X," % b for b in data[i:i + 12]) + "\n")
    f.write("};\n")


with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("/* generated by gen_ds4_vectors.py - do not edit */\n")
    emit(f, "V_DS4_IN_USB", usb_in)
    emit(f, "V_DS4_IN_USB_FULL_BATT", usb_in_full)
    emit(f, "V_DS4_IN_BT", bt_in)
    emit(f, "V_DS4_OUT_USB", usb_out)
    emit(f, "V_DS4_OUT_BT", bt_out)
    emit(f, "V_DS4_CAL_USB", cal_usb)
    emit(f, "V_DS4_CAL_BT", cal_bt)
    f.write("\n/* gyro scale the calibration above should produce */\n")
    for i, (numer, denom) in enumerate(expect):
        f.write("#define DS4_CAL_NUMER_%d %d\n" % (i, numer))
        f.write("#define DS4_CAL_DENOM_%d %d\n" % (i, denom))

print("wrote", OUT)
print("usb_in", len(usb_in), "bt_in", len(bt_in), "usb_out", len(usb_out),
      "bt_out", len(bt_out))
print("gyro expect", expect)

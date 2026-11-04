# -*- coding: utf-8 -*-
"""Expected adaptive-trigger blocks, from the reference rather than from us.

A transcription of Nielk1's TriggerEffectGenerator:

    https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db

which is where the packing in ds_protocol.c came from in the first place.
Written out again here so the test compares two independent readings of the
same source. Vectors taken from the implementation would only prove the
implementation had not changed.

Run this and paste the output into vectors.h, replacing the trigger block.
"""

OFF, FEEDBACK, WEAPON, VIBRATION = 0x05, 0x21, 0x25, 0x26
BOW, GALLOPING, MACHINE = 0x22, 0x23, 0x27


def off():
    return [OFF] + [0] * 10


def multi_feedback(strength):
    """strength[10], each 0-8. Zero means the zone is not active at all."""
    assert len(strength) == 10
    if not any(strength):
        return off()
    force, active = 0, 0
    for i, s in enumerate(strength):
        if s > 0:
            force |= ((s - 1) & 0x07) << (3 * i)
            active |= 1 << i
    return [FEEDBACK,
            active & 0xFF, (active >> 8) & 0xFF,
            force & 0xFF, (force >> 8) & 0xFF,
            (force >> 16) & 0xFF, (force >> 24) & 0xFF,
            0, 0, 0, 0]


def multi_vibration(frequency, amplitude):
    assert len(amplitude) == 10
    if frequency == 0 or not any(amplitude):
        return off()
    zones, active = 0, 0
    for i, a in enumerate(amplitude):
        if a > 0:
            zones |= ((a - 1) & 0x07) << (3 * i)
            active |= 1 << i
    return [VIBRATION,
            active & 0xFF, (active >> 8) & 0xFF,
            zones & 0xFF, (zones >> 8) & 0xFF,
            (zones >> 16) & 0xFF, (zones >> 24) & 0xFF,
            0, 0, frequency, 0]


def feedback(position, strength):
    s = [0] * 10
    for i in range(position, 10):
        s[i] = strength
    return multi_feedback(s)


def vibration(position, amplitude, frequency):
    a = [0] * 10
    for i in range(position, 10):
        a[i] = amplitude
    return multi_vibration(frequency, a)


def weapon(start, end, strength):
    zones = (1 << start) | (1 << end)
    return [WEAPON, zones & 0xFF, (zones >> 8) & 0xFF, strength - 1] + [0] * 7


def slope(start, end, start_strength, end_strength):
    """Linear ramp between two zones, then held to the end of the travel."""
    s = [0] * 10
    step = 1.0 * (end_strength - start_strength) / (end - start)
    for i in range(start, 10):
        if i <= end:
            # C# Math.Round is banker's rounding; every case used here lands
            # on an exact value, so it does not come up.
            s[i] = int(round(start_strength + step * (i - start)))
        else:
            s[i] = end_strength
    return multi_feedback(s)


def bow(start, end, strength, snap_force):
    zones = (1 << start) | (1 << end)
    pair = (((strength - 1) & 0x07) << 0) | (((snap_force - 1) & 0x07) << 3)
    return [BOW, zones & 0xFF, (zones >> 8) & 0xFF,
            pair & 0xFF, (pair >> 8) & 0xFF] + [0] * 6


def galloping(start, end, first_foot, second_foot, frequency):
    zones = (1 << start) | (1 << end)
    ratio = ((second_foot & 0x07) << 0) | ((first_foot & 0x07) << 3)
    return [GALLOPING, zones & 0xFF, (zones >> 8) & 0xFF,
            ratio & 0xFF, frequency] + [0] * 6


def machine(start, end, amp_a, amp_b, frequency, period):
    zones = (1 << start) | (1 << end)
    pair = ((amp_a & 0x07) << 0) | ((amp_b & 0x07) << 3)
    return [MACHINE, zones & 0xFF, (zones >> 8) & 0xFF,
            pair & 0xFF, frequency, period] + [0] * 5


CASES = [
    ("V_TRIG_OFF", off()),
    ("V_TRIG_FB_3_6", feedback(3, 6)),
    ("V_TRIG_WPN_3_6_8", weapon(3, 6, 8)),
    ("V_TRIG_VIB_2_6_30", vibration(2, 6, 30)),
    ("V_TRIG_SLOPE_1_8_2_7", slope(1, 8, 2, 7)),
    ("V_TRIG_MULTI_FB", multi_feedback([0, 0, 3, 3, 6, 6, 8, 0, 0, 1])),
    ("V_TRIG_MULTI_VIB", multi_vibration(35, [0, 2, 4, 6, 8, 0, 0, 0, 0, 5])),
    ("V_TRIG_BOW_1_5_6_7", bow(1, 5, 6, 7)),
    ("V_TRIG_GALLOP_1_8_2_5_20", galloping(1, 8, 2, 5, 20)),
    ("V_TRIG_MACHINE_1_9_3_6_25_4", machine(1, 9, 3, 6, 25, 4)),
]

for name, block in CASES:
    body = ", ".join("0x%02X" % b for b in block)
    print("static const uint8_t %s[] = {" % name)
    print("  %s," % body)
    print("};")

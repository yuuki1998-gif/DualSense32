# -*- coding: utf-8 -*-
"""
Generates examples/SelfTest/HciPackets/vectors.h.

Every byte string below is written out from the Bluetooth Core Specification
field lists (Vol 4 Part E), transcribed independently of ds_hci.c. If either
side is wrong the self-test fails, which is the whole point -- generating the
vectors from the implementation would only prove it agrees with itself.

Field orders used, straight from the spec:
  command packet          opcode(2 LE)  param_total_length(1)  params
  event packet            event_code(1) param_total_length(1)  params
  ACL packet              handle|pb<<12|bc<<14 (2 LE)  data_total_length(2 LE)
  BD_ADDR                 6 bytes, little endian (reverse of how it is printed)
"""
import io
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors.h")

vectors = []


def add(name, data):
    vectors.append((name, bytes(data)))


def opcode(ogf, ocf):
    return (ogf << 10) | ocf


def cmd(ogf, ocf, params=b""):
    assert len(params) <= 255
    return struct.pack("<HB", opcode(ogf, ocf), len(params)) + params


def evt(code, params):
    assert len(params) <= 255
    return struct.pack("<BB", code, len(params)) + params


def acl(handle, pb, bc, data):
    return struct.pack("<HH", handle | (pb << 12) | (bc << 14), len(data)) + data


# A neutral test address. Printed as 11:22:33:44:55:66, so on the wire it is
# reversed.
ADDR_STR = "11:22:33:44:55:66"
ADDR = bytes([0x66, 0x55, 0x44, 0x33, 0x22, 0x11])
LINK_KEY = bytes(range(16))

add("V_ADDR", ADDR)
add("V_LINK_KEY", LINK_KEY)

# --------------------------------------------------------------- commands ---

# OGF 0x03 Controller & Baseband, OCF 0x0003 Reset, no parameters.
add("V_CMD_RESET", cmd(0x03, 0x0003))

# OGF 0x04 Informational, OCF 0x0001 Read Local Version Information.
add("V_CMD_READ_LOCAL_VERSION", cmd(0x04, 0x0001))

# Write Scan Enable, Scan_Enable(1). 0x03 = inquiry scan + page scan.
add("V_CMD_SCAN_BOTH", cmd(0x03, 0x001A, bytes([0x03])))

# Set Event Mask, Event_Mask(8 LE). All ones enables everything including the
# secure simple pairing events, which the spec default leaves masked off.
add("V_CMD_EVENT_MASK_ALL", cmd(0x03, 0x0001, b"\xFF" * 8))

# ...and the spec's own default value, to prove the byte order.
add("V_CMD_EVENT_MASK_DEFAULT",
    cmd(0x03, 0x0001, struct.pack("<Q", 0x00001FFFFFFFFFFF)))

# Write Class of Device, Class_Of_Device(3 LE). 0x002508 = peripheral/gamepad.
add("V_CMD_COD", cmd(0x03, 0x0024, struct.pack("<I", 0x002508)[:3]))

# Write Simple Pairing Mode, Simple_Pairing_Mode(1).
add("V_CMD_SSP_ON", cmd(0x03, 0x0056, bytes([0x01])))

# Inquiry: LAP(3 LE) Inquiry_Length(1) Num_Responses(1).
# GIAC 0x9E8B33 goes out as 33 8B 9E.
add("V_CMD_INQUIRY",
    cmd(0x01, 0x0001, struct.pack("<I", 0x9E8B33)[:3] + bytes([8, 0])))

# Create Connection: BD_ADDR(6) Packet_Type(2 LE) Page_Scan_Repetition_Mode(1)
#                    Reserved(1) Clock_Offset(2 LE) Allow_Role_Switch(1)
add("V_CMD_CREATE_CONN",
    cmd(0x01, 0x0005,
        ADDR + struct.pack("<H", 0xCC18) + bytes([0x01, 0x00]) +
        struct.pack("<H", 0x0000) + bytes([0x01])))

# Disconnect: Connection_Handle(2 LE) Reason(1). 0x13 = remote user terminated.
add("V_CMD_DISCONNECT",
    cmd(0x01, 0x0006, struct.pack("<H", 0x000C) + bytes([0x13])))

# IO Capability Request Reply: BD_ADDR(6) IO_Capability(1) OOB_Data_Present(1)
#                              Authentication_Requirements(1)
# 0x03 NoInputNoOutput, no OOB, 0x04 no MITM + general bonding.
add("V_CMD_IOCAP_REPLY",
    cmd(0x01, 0x002B, ADDR + bytes([0x03, 0x00, 0x04])))

# Link Key Request Reply: BD_ADDR(6) Link_Key(16)
add("V_CMD_LINK_KEY_REPLY", cmd(0x01, 0x000B, ADDR + LINK_KEY))

# Link Key Request Negative Reply: BD_ADDR(6)
add("V_CMD_LINK_KEY_NEG_REPLY", cmd(0x01, 0x000C, ADDR))

# User Confirmation Request Reply / Negative Reply: BD_ADDR(6)
add("V_CMD_USER_CONF_ACCEPT", cmd(0x01, 0x002C, ADDR))
add("V_CMD_USER_CONF_REJECT", cmd(0x01, 0x002D, ADDR))

# Authentication Requested: Connection_Handle(2 LE)
add("V_CMD_AUTH_REQ", cmd(0x01, 0x0011, struct.pack("<H", 0x000C)))

# Set Connection Encryption: Connection_Handle(2 LE) Encryption_Enable(1)
add("V_CMD_SET_ENCRYPTION",
    cmd(0x01, 0x0013, struct.pack("<H", 0x000C) + bytes([0x01])))

# Write Local Name: Local_Name(248), NOT the string length. Zero padded.
name = b"DualSense32"
add("V_CMD_LOCAL_NAME", cmd(0x03, 0x0013, name + b"\x00" * (248 - len(name))))

# ----------------------------------------------------------------- events ---

# Command Complete: Num_HCI_Command_Packets(1) Opcode(2 LE) Return_Parameters
# Return parameters of Read Local Version Information:
#   Status(1) HCI_Version(1) HCI_Revision(2) LMP_Version(1)
#   Manufacturer_Name(2) LMP_Subversion(2)
# The numbers are the ones the UGREEN dongle actually reports: HCI version 14
# (Bluetooth 6.0) and manufacturer 2279.
add("V_EVT_CC_LOCAL_VERSION",
    evt(0x0E,
        bytes([0x01]) + struct.pack("<H", opcode(0x04, 0x0001)) +
        bytes([0x00, 14]) + struct.pack("<H", 0x0000) + bytes([14]) +
        struct.pack("<H", 2279) + struct.pack("<H", 0x0000)))

# Command Status: Status(1) Num_HCI_Command_Packets(1) Opcode(2 LE)
add("V_EVT_CS_CREATE_CONN",
    evt(0x0F, bytes([0x00, 0x01]) + struct.pack("<H", opcode(0x01, 0x0005))))

# Connection Complete: Status(1) Connection_Handle(2 LE) BD_ADDR(6)
#                      Link_Type(1) Encryption_Enabled(1)
add("V_EVT_CONN_COMPLETE",
    evt(0x03, bytes([0x00]) + struct.pack("<H", 0x000C) + ADDR +
        bytes([0x01, 0x00])))

# Connection Request: BD_ADDR(6) Class_of_Device(3 LE) Link_Type(1)
add("V_EVT_CONN_REQUEST",
    evt(0x04, ADDR + struct.pack("<I", 0x002508)[:3] + bytes([0x01])))

# Disconnection Complete: Status(1) Connection_Handle(2 LE) Reason(1)
add("V_EVT_DISCONN",
    evt(0x05, bytes([0x00]) + struct.pack("<H", 0x000C) + bytes([0x13])))

# Inquiry Result (0x02): Num_Responses(1), then per response
#   BD_ADDR(6) Page_Scan_Repetition_Mode(1) Reserved(1) Reserved(1)
#   Class_of_Device(3 LE) Clock_Offset(2 LE)               -> 14 bytes
add("V_EVT_INQ_RESULT",
    evt(0x02,
        bytes([0x01]) + ADDR + bytes([0x01, 0x00, 0x00]) +
        struct.pack("<I", 0x002508)[:3] + struct.pack("<H", 0x1234)))

# Inquiry Result with RSSI (0x22): one Reserved byte fewer, RSSI on the end
#   BD_ADDR(6) Page_Scan_Repetition_Mode(1) Reserved(1)
#   Class_of_Device(3 LE) Clock_Offset(2 LE) RSSI(1)       -> also 14 bytes
# Same total length, different offsets. This pair is the trap.
add("V_EVT_INQ_RESULT_RSSI",
    evt(0x22,
        bytes([0x01]) + ADDR + bytes([0x01, 0x00]) +
        struct.pack("<I", 0x002508)[:3] + struct.pack("<H", 0x1234) +
        bytes([0xCE])))          # -50 dBm as a signed byte

# Two responses in one Inquiry Result, to exercise the stride.
addr2 = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])
add("V_EVT_INQ_RESULT_2",
    evt(0x02,
        bytes([0x02]) +
        ADDR + bytes([0x01, 0x00, 0x00]) + struct.pack("<I", 0x002508)[:3] +
        struct.pack("<H", 0x1234) +
        addr2 + bytes([0x02, 0x00, 0x00]) + struct.pack("<I", 0x240404)[:3] +
        struct.pack("<H", 0x5678)))
add("V_ADDR2", addr2)

# Link Key Notification: BD_ADDR(6) Link_Key(16) Key_Type(1)
add("V_EVT_LINK_KEY_NOTIF", evt(0x18, ADDR + LINK_KEY + bytes([0x05])))

# User Confirmation Request: BD_ADDR(6) Numeric_Value(4 LE)
add("V_EVT_USER_CONF", evt(0x33, ADDR + struct.pack("<I", 123456)))

# IO Capability Request / Link Key Request: BD_ADDR(6)
add("V_EVT_IOCAP_REQ", evt(0x31, ADDR))
add("V_EVT_LINK_KEY_REQ", evt(0x17, ADDR))

# -------------------------------------------------------------------- ACL ---

# Header only: handle 0x000C, pb = 0b10 (first, flushable), bc = 0, 8 bytes.
add("V_ACL_HDR", acl(0x000C, 0x02, 0x00, b"\x00" * 8)[:4])

# A complete small L2CAP packet inside one ACL packet. L2CAP header is
# length(2 LE) then channel id(2 LE); length counts the payload only.
l2cap_small = struct.pack("<HH", 4, 0x0041) + bytes([0xA1, 0x31, 0xAA, 0xBB])
add("V_ACL_ONE", acl(0x000C, 0x02, 0x00, l2cap_small))
add("V_L2CAP_SMALL", l2cap_small)

# A 100 byte L2CAP payload split across two ACL fragments of 60 and 44 bytes.
payload = bytes((i * 7 + 3) & 0xFF for i in range(100))
l2cap_big = struct.pack("<HH", len(payload), 0x0041) + payload
assert len(l2cap_big) == 104
add("V_ACL_FRAG1", acl(0x000C, 0x02, 0x00, l2cap_big[:60]))
add("V_ACL_FRAG2", acl(0x000C, 0x01, 0x00, l2cap_big[60:]))
add("V_L2CAP_BIG", l2cap_big)

# ------------------------------------------------------------------ write ---

lines = ["/* generated by gen_hci_vectors.py - do not edit */\n"]
for name, data in vectors:
    lines.append("static const uint8_t %s[] = {\n" % name)
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("  " + " ".join("0x%02X," % b for b in chunk) + "\n")
    lines.append("};\n")
lines.append('#define V_ADDR_STR "%s"\n' % ADDR_STR)

io.open(OUT, "w", encoding="utf-8", newline="").write("".join(lines))
print("wrote %s, %d vectors" % (OUT, len(vectors)))
for name, data in vectors:
    print("  %-28s %3d bytes" % (name, len(data)))

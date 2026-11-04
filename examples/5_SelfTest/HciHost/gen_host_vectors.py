# -*- coding: utf-8 -*-
"""
Generates examples/SelfTest/HciHost/vectors.h -- a whole HCI conversation.

The events are what a controller and a DualSense actually say, written out
from the Bluetooth Core Specification field lists. The commands are what a
correct host must say back, decided from the specification rather than from
ds_hci_host.c. The self-test replays the conversation turn by turn and
compares byte for byte.
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


def cmd(op, params=b""):
    return struct.pack("<HB", op, len(params)) + params


def evt(code, params):
    return struct.pack("<BB", code, len(params)) + params


def cc(op, ret=b"\x00"):
    """Command Complete: Num_HCI_Command_Packets(1) Opcode(2) Return_Params."""
    return evt(0x0E, bytes([0x01]) + struct.pack("<H", op) + ret)


def cs(op, status=0x00):
    """Command Status: Status(1) Num_HCI_Command_Packets(1) Opcode(2)."""
    return evt(0x0F, bytes([status, 0x01]) + struct.pack("<H", op))


OP_RESET = opcode(0x03, 0x0003)
OP_READ_VERSION = opcode(0x04, 0x0001)
OP_READ_BUFSIZE = opcode(0x04, 0x0005)
OP_READ_BDADDR = opcode(0x04, 0x0009)
OP_READ_LOCAL_CMDS = opcode(0x04, 0x0002)
OP_READ_LOCAL_FEAT = opcode(0x04, 0x0003)
OP_EVENT_MASK_P2 = opcode(0x03, 0x0063)
OP_WRITE_SECURE_CONN = opcode(0x03, 0x007A)
OP_WRITE_INQ_MODE = opcode(0x03, 0x0045)
OP_WRITE_PAGE_TIMEOUT = opcode(0x03, 0x0018)
OP_WRITE_PSCAN_ACT = opcode(0x03, 0x001C)
OP_WRITE_PSCAN_TYPE = opcode(0x03, 0x0047)
OP_READ_REMOTE_FEAT = opcode(0x01, 0x001B)
OP_READ_REMOTE_EXT = opcode(0x01, 0x001C)
OP_READ_ENC_KEY_SIZE = opcode(0x05, 0x0008)
OP_EVENT_MASK = opcode(0x03, 0x0001)
OP_LINK_POLICY = opcode(0x02, 0x000F)
OP_WRITE_COD = opcode(0x03, 0x0024)
OP_WRITE_NAME = opcode(0x03, 0x0013)
OP_WRITE_SSP = opcode(0x03, 0x0056)
OP_WRITE_SCAN = opcode(0x03, 0x001A)
OP_CREATE_CONN = opcode(0x01, 0x0005)
OP_ACCEPT_CONN = opcode(0x01, 0x0009)
OP_AUTH_REQ = opcode(0x01, 0x0011)
OP_SET_ENC = opcode(0x01, 0x0013)
OP_LINK_KEY_REPLY = opcode(0x01, 0x000B)
OP_LINK_KEY_NEG = opcode(0x01, 0x000C)
OP_IOCAP_REPLY = opcode(0x01, 0x002B)
OP_USER_CONF = opcode(0x01, 0x002C)
OP_DISCONNECT = opcode(0x01, 0x0006)

# The controller's own address, and the DualSense we are connecting to.
LOCAL = bytes([0x98, 0x15, 0x06, 0xBB, 0xBC, 0x2C])
PEER = bytes([0x64, 0x12, 0x23, 0xFA, 0xA6, 0x24])
KEY = bytes([0xA0 + i for i in range(16)])
HANDLE = 0x000C
COD_GAMEPAD = 0x002508

add("V_LOCAL", LOCAL)
add("V_PEER", PEER)
add("V_KEY", KEY)

# =========================================================== setup sequence ==
# Nine steps, each a command out and a Command Complete back. The defaults
# baked into ds_hci_host_init are: name "DualSense32", class of device
# 0x000104, page scan only, no input no output, no MITM general bonding.

add("V_CMD_1_RESET", cmd(OP_RESET))
add("V_EVT_1_RESET_OK", cc(OP_RESET))

add("V_CMD_2_VERSION", cmd(OP_READ_VERSION))
# Status(1) HCI_Version(1) HCI_Revision(2) LMP_Version(1) Manufacturer(2)
# LMP_Subversion(2) -- the UGREEN dongle's actual numbers.
add("V_EVT_2_VERSION_OK",
    cc(OP_READ_VERSION,
       bytes([0x00, 14]) + struct.pack("<H", 0) + bytes([14]) +
       struct.pack("<H", 2279) + struct.pack("<H", 0)))

add("V_CMD_3_BUFSIZE", cmd(OP_READ_BUFSIZE))
# Status(1) ACL_Data_Packet_Length(2) Sync_Data_Packet_Length(1)
# Total_Num_ACL(2) Total_Num_Sync(2)
add("V_EVT_3_BUFSIZE_OK",
    cc(OP_READ_BUFSIZE,
       bytes([0x00]) + struct.pack("<H", 1021) + bytes([8]) +
       struct.pack("<H", 4) + struct.pack("<H", 8)))

# Read Local Supported Commands returns a 64 byte bitmap, Read Local
# Supported Features an 8 byte one. Neither is inspected; asking is how a host
# finds out what it may ask for next.
add("V_CMD_2B_LOCAL_CMDS", cmd(OP_READ_LOCAL_CMDS))
add("V_EVT_2B_OK", cc(OP_READ_LOCAL_CMDS, bytes([0x00]) + b"\xFF" * 64))

add("V_CMD_2C_LOCAL_FEAT", cmd(OP_READ_LOCAL_FEAT))
add("V_EVT_2C_OK",
    cc(OP_READ_LOCAL_FEAT,
       bytes([0x00, 0xBF, 0xEE, 0x49, 0xFA, 0xDB, 0xBF, 0x7B, 0x87])))

add("V_CMD_4_BDADDR", cmd(OP_READ_BDADDR))
add("V_EVT_4_BDADDR_OK", cc(OP_READ_BDADDR, bytes([0x00]) + LOCAL))

# All ones: the spec default masks off the secure simple pairing events.
add("V_CMD_5_EVENT_MASK", cmd(OP_EVENT_MASK, b"\xFF" * 8))
add("V_EVT_5_OK", cc(OP_EVENT_MASK))

# Page 2 defaults to all zeroes, which masks off the secure connections
# events. All ones, for the same reason page 1 is.
add("V_CMD_5C_EVENT_MASK_P2", cmd(OP_EVENT_MASK_P2, b"\xFF" * 8))
add("V_EVT_5C_OK", cc(OP_EVENT_MASK_P2))

# Write Default Link Policy Settings: role switch + sniff.
add("V_CMD_5B_POLICY", cmd(OP_LINK_POLICY, struct.pack("<H", 0x0005)))
add("V_EVT_5B_OK", cc(OP_LINK_POLICY))

add("V_CMD_6_COD", cmd(OP_WRITE_COD, struct.pack("<I", 0x000104)[:3]))
add("V_EVT_6_OK", cc(OP_WRITE_COD))

name = b"DualSense32"
add("V_CMD_7_NAME", cmd(OP_WRITE_NAME, name + b"\x00" * (248 - len(name))))
add("V_EVT_7_OK", cc(OP_WRITE_NAME))

add("V_CMD_8_SSP", cmd(OP_WRITE_SSP, bytes([0x01])))
add("V_EVT_8_OK", cc(OP_WRITE_SSP))

add("V_CMD_8B_SECURE_CONN", cmd(OP_WRITE_SECURE_CONN, bytes([0x01])))
add("V_EVT_8B_OK", cc(OP_WRITE_SECURE_CONN))

add("V_CMD_8C_INQ_MODE", cmd(OP_WRITE_INQ_MODE, bytes([0x02])))   # with RSSI+EIR
add("V_EVT_8C_OK", cc(OP_WRITE_INQ_MODE))

add("V_CMD_8D_PAGE_TIMEOUT",
    cmd(OP_WRITE_PAGE_TIMEOUT, struct.pack("<H", 0x2000)))
add("V_EVT_8D_OK", cc(OP_WRITE_PAGE_TIMEOUT))

# Page Scan Activity: Interval(2) Window(2), both in 0.625 ms units.
add("V_CMD_8E_PSCAN_ACT",
    cmd(OP_WRITE_PSCAN_ACT, struct.pack("<HH", 0x0100, 0x0012)))
add("V_EVT_8E_OK", cc(OP_WRITE_PSCAN_ACT))

add("V_CMD_8F_PSCAN_TYPE", cmd(OP_WRITE_PSCAN_TYPE, bytes([0x01])))  # interlaced
add("V_EVT_8F_OK", cc(OP_WRITE_PSCAN_TYPE))

add("V_CMD_9_SCAN", cmd(OP_WRITE_SCAN, bytes([0x02])))   # page scan only
add("V_EVT_9_OK", cc(OP_WRITE_SCAN))

# A rejected setup step, to prove the host stops instead of carrying on.
add("V_EVT_RESET_FAIL", cc(OP_RESET, bytes([0x12])))     # invalid parameters

# ================================================================= paging ====

# Create Connection: BD_ADDR(6) Packet_Type(2) PSRM(1) Reserved(1)
#                    Clock_Offset(2) Allow_Role_Switch(1)
add("V_CMD_CREATE_CONN",
    cmd(OP_CREATE_CONN,
        PEER + struct.pack("<H", 0xCC18) + bytes([0x01, 0x00]) +
        struct.pack("<H", 0x0000) + bytes([0x01])))
add("V_EVT_CREATE_CONN_PENDING", cs(OP_CREATE_CONN))
add("V_EVT_CREATE_CONN_REFUSED", cs(OP_CREATE_CONN, 0x0C))   # command disallowed

# Connection Complete: Status(1) Handle(2) BD_ADDR(6) Link_Type(1) Encryption(1)
add("V_EVT_CONN_COMPLETE",
    evt(0x03, bytes([0x00]) + struct.pack("<H", HANDLE) + PEER +
        bytes([0x01, 0x00])))
add("V_EVT_CONN_FAILED",
    evt(0x03, bytes([0x04]) + struct.pack("<H", 0) + PEER + bytes([0x01, 0x00])))

# A link comes up and the features are read before anything else happens.
# Read Remote Supported Features Complete: Status(1) Handle(2) Features(8).
add("V_CMD_REMOTE_FEAT", cmd(OP_READ_REMOTE_FEAT, struct.pack("<H", HANDLE)))
add("V_EVT_REMOTE_FEAT",
    evt(0x0B, bytes([0x00]) + struct.pack("<H", HANDLE) +
        bytes([0xBF, 0x3A, 0x8D, 0xFE, 0xDB, 0xFF, 0x7B, 0x87])))

# Read Remote Extended Features Complete: Status(1) Handle(2) Page(1)
#                                          Max_Page(1) Features(8).
add("V_CMD_REMOTE_EXT",
    cmd(OP_READ_REMOTE_EXT, struct.pack("<H", HANDLE) + bytes([0x01])))
add("V_EVT_REMOTE_EXT",
    evt(0x23, bytes([0x00]) + struct.pack("<H", HANDLE) + bytes([0x01, 0x02]) +
        bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])))

add("V_CMD_AUTH_REQ", cmd(OP_AUTH_REQ, struct.pack("<H", HANDLE)))

# Asked once the link is encrypted. A short key is a weak link, and the only
# moment to find out is now.
add("V_CMD_ENC_KEY_SIZE",
    cmd(OP_READ_ENC_KEY_SIZE, struct.pack("<H", HANDLE)))

# ================================================================ pairing ====

# Link Key Request: BD_ADDR(6)
add("V_EVT_LINK_KEY_REQ", evt(0x17, PEER))
add("V_CMD_LINK_KEY_NEG", cmd(OP_LINK_KEY_NEG, PEER))
add("V_CMD_LINK_KEY_REPLY", cmd(OP_LINK_KEY_REPLY, PEER + KEY))

# IO Capability Request: BD_ADDR(6)
add("V_EVT_IOCAP_REQ", evt(0x31, PEER))
# Reply: BD_ADDR(6) IO_Capability(1) OOB(1) Authentication_Requirements(1)
add("V_CMD_IOCAP_REPLY", cmd(OP_IOCAP_REPLY, PEER + bytes([0x03, 0x00, 0x04])))

# IO Capability Response: BD_ADDR(6) IO_Capability(1) OOB(1) Auth_Req(1)
# Nothing is expected back for this one.
add("V_EVT_IOCAP_RESPONSE", evt(0x32, PEER + bytes([0x03, 0x00, 0x04])))

# User Confirmation Request: BD_ADDR(6) Numeric_Value(4)
add("V_EVT_USER_CONF", evt(0x33, PEER + struct.pack("<I", 123456)))
add("V_CMD_USER_CONF_ACCEPT", cmd(OP_USER_CONF, PEER))

# Simple Pairing Complete: Status(1) BD_ADDR(6)
add("V_EVT_SSP_COMPLETE", evt(0x36, bytes([0x00]) + PEER))

# Link Key Notification: BD_ADDR(6) Link_Key(16) Key_Type(1)
add("V_EVT_LINK_KEY_NOTIF", evt(0x18, PEER + KEY + bytes([0x05])))

# Authentication Complete: Status(1) Handle(2)
add("V_EVT_AUTH_COMPLETE", evt(0x06, bytes([0x00]) + struct.pack("<H", HANDLE)))
add("V_EVT_AUTH_FAILED", evt(0x06, bytes([0x05]) + struct.pack("<H", HANDLE)))

add("V_CMD_SET_ENC",
    cmd(OP_SET_ENC, struct.pack("<H", HANDLE) + bytes([0x01])))

# Encryption Change: Status(1) Handle(2) Encryption_Enabled(1)
add("V_EVT_ENC_ON",
    evt(0x08, bytes([0x00]) + struct.pack("<H", HANDLE) + bytes([0x01])))

# =============================================================== inbound =====

# Connection Request: BD_ADDR(6) Class_of_Device(3) Link_Type(1)
add("V_EVT_CONN_REQUEST",
    evt(0x04, PEER + struct.pack("<I", COD_GAMEPAD)[:3] + bytes([0x01])))
# Accept Connection Request: BD_ADDR(6) Role(1). 0x01 stays the peripheral.
add("V_CMD_ACCEPT_CONN", cmd(OP_ACCEPT_CONN, PEER + bytes([0x01])))
# Reject Connection Request (OGF 0x01, OCF 0x000A): BD_ADDR(6) Reason(1).
# 0x0F is "Connection Rejected due to Unacceptable BD_ADDR" (Vol 1 Part F).
add("V_CMD_REJECT_CONN", cmd(opcode(0x01, 0x000A), PEER + bytes([0x0F])))

# ============================================================ housekeeping ===

# Number Of Completed Packets: Num_Handles(1) then Handle(2) Num_Completed(2)
add("V_EVT_COMPLETED_2",
    evt(0x13, bytes([0x01]) + struct.pack("<H", HANDLE) +
        struct.pack("<H", 2)))

# Disconnection Complete: Status(1) Handle(2) Reason(1)
add("V_EVT_DISCONN",
    evt(0x05, bytes([0x00]) + struct.pack("<H", HANDLE) + bytes([0x13])))

# Disconnect, as fail() sends it for a handle that is still live: Handle(2)
# Reason(1). 0x13 is "remote user terminated connection".
add("V_CMD_DISCONNECT",
    cmd(OP_DISCONNECT, struct.pack("<H", HANDLE) + bytes([0x13])))

# Inquiry Result with RSSI, one gamepad.
add("V_EVT_INQ_GAMEPAD",
    evt(0x22,
        bytes([0x01]) + PEER + bytes([0x01, 0x00]) +
        struct.pack("<I", COD_GAMEPAD)[:3] + struct.pack("<H", 0x1234) +
        bytes([0xCE])))
# Inquiry Complete: Status(1)
add("V_EVT_INQ_COMPLETE", evt(0x01, bytes([0x00])))

# An ACL packet carrying a DualSense input report on the HID interrupt
# channel: L2CAP length(2) cid(2) then 0xA1 and the report.
report = bytes([0xA1, 0x31]) + bytes((i * 3 + 1) & 0xFF for i in range(77))
l2cap = struct.pack("<HH", len(report), 0x0041) + report
acl_in = struct.pack("<HH", HANDLE | (0x02 << 12), len(l2cap)) + l2cap
add("V_ACL_IN", acl_in)
add("V_HID_REPORT", report)

# ------------------------------------------------------------------ write ---

lines = ["/* generated by gen_host_vectors.py - do not edit */\n"]
for nm, data in vectors:
    lines.append("static const uint8_t %s[] = {\n" % nm)
    for i in range(0, len(data), 12):
        lines.append("  " + " ".join("0x%02X," % b for b in data[i:i + 12]) +
                     "\n")
    lines.append("};\n")
lines.append("#define V_HANDLE 0x%04X\n" % HANDLE)
lines.append("#define V_COD_GAMEPAD 0x%06XUL\n" % COD_GAMEPAD)

io.open(OUT, "w", encoding="utf-8", newline="").write("".join(lines))
print("wrote %s, %d vectors" % (OUT, len(vectors)))

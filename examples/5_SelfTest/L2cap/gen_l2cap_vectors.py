# -*- coding: utf-8 -*-
"""
Generates examples/SelfTest/L2cap/vectors.h.

Written out from the Bluetooth Core Specification, Vol 3 Part A (L2CAP),
independently of ds_l2cap.c.

Two kinds of vector:
  V_IN_*   complete signalling packets fed to the implementation. The
           identifiers here are ours to choose, and the test asserts that
           replies echo them.
  V_OUT_*  the BODY of a signalling packet the implementation must emit --
           no code and no identifier, because the identifier depends on how
           many requests have gone before and pinning it would test the
           counter rather than the protocol.

Signalling packet: Code(1) Identifier(1) Length(2 LE) Data
"""
import io
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors.h")

vectors = []


def add(name, data):
    vectors.append((name, bytes(data)))


def sig(code, ident, body):
    return struct.pack("<BBH", code, ident, len(body)) + body


def u16(v):
    return struct.pack("<H", v)


# Channel ids. Ours start at 0x0040 because everything below that is
# reserved; the remote's are picked to be obviously different.
LCID_CTRL, LCID_INTR = 0x0040, 0x0041
RCID_CTRL, RCID_INTR = 0x0070, 0x0071
PSM_CTRL, PSM_INTR = 0x0011, 0x0013
MTU = 672

# Identifiers we use in the packets we feed in.
ID_CFG_REQ_CTRL = 0x21
ID_CFG_REQ_INTR = 0x22
ID_CONN_REQ_IN = 0x23
ID_DISCONN_REQ = 0x24
ID_ECHO = 0x25
ID_INFO = 0x26
ID_BAD_CID = 0x27
ID_UNKNOWN_CODE = 0x28
ID_BAD_PSM = 0x29
ID_UNKNOWN_OPT = 0x2A

MTU_OPT = bytes([0x01, 0x02]) + u16(MTU)

# ============================================================ outbound =======

# Connection Request body: PSM(2) Source CID(2)
add("V_OUT_CONN_REQ_CTRL", u16(PSM_CTRL) + u16(LCID_CTRL))
add("V_OUT_CONN_REQ_INTR", u16(PSM_INTR) + u16(LCID_INTR))

# Configuration Request body: Destination CID(2) Flags(2) Options
add("V_OUT_CONFIG_REQ_CTRL", u16(RCID_CTRL) + u16(0) + MTU_OPT)
add("V_OUT_CONFIG_REQ_INTR", u16(RCID_INTR) + u16(0) + MTU_OPT)

# Configuration Response body: Source CID(2) Flags(2) Result(2) Options
# The source cid is the requester's endpoint, so replying to the remote means
# naming the remote's cid.
add("V_OUT_CONFIG_RSP_CTRL", u16(RCID_CTRL) + u16(0) + u16(0x0000))
add("V_OUT_CONFIG_RSP_INTR", u16(RCID_INTR) + u16(0) + u16(0x0000))

# Connection Response body: Destination CID(2) Source CID(2) Result(2) Status(2)
add("V_OUT_CONN_RSP_ACCEPT",
    u16(LCID_CTRL) + u16(RCID_CTRL) + u16(0x0000) + u16(0x0000))
# PSM not supported: no destination cid to give.
add("V_OUT_CONN_RSP_NO_PSM",
    u16(0x0000) + u16(RCID_CTRL) + u16(0x0002) + u16(0x0000))

# The same accept, but after our own half-built attempt was given up: the
# freed channel id is not reused, so this one carries the next one along.
add("V_OUT_CONN_RSP_ACCEPT_2",
    u16(LCID_INTR) + u16(RCID_CTRL) + u16(0x0000) + u16(0x0000))
# No channel to spare, because the one for this PSM is already open.
add("V_OUT_CONN_RSP_NO_RESOURCES",
    u16(0x0000) + u16(RCID_CTRL) + u16(0x0004) + u16(0x0000))

# Disconnection Response body echoes the request: DCID(2) SCID(2)
add("V_OUT_DISCONN_RSP", u16(LCID_CTRL) + u16(RCID_CTRL))
# Disconnection Request we send: DCID(2) is theirs, SCID(2) is ours
add("V_OUT_DISCONN_REQ", u16(RCID_CTRL) + u16(LCID_CTRL))

# Command Reject body: Reason(2) then reason-specific data
add("V_OUT_REJECT_INVALID_CID", u16(0x0002) + u16(0x0099) + u16(0x0000))
add("V_OUT_REJECT_NOT_UNDERSTOOD", u16(0x0000))

# Information Response body: Info Type(2) Result(2). 0x0001 = not supported.
add("V_OUT_INFO_RSP", u16(0x0002) + u16(0x0001))

# Configuration Response rejecting an option we do not implement. Retransmission
# and flow control, option type 0x04, nine bytes of parameters.
RETRANSMIT_OPT = bytes([0x04, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00])
add("V_OUT_CONFIG_RSP_UNKNOWN",
    u16(RCID_CTRL) + u16(0) + u16(0x0003) + RETRANSMIT_OPT)

# After they refuse 672 and offer 185, we ask again for exactly 185.
add("V_OUT_CONFIG_REQ_RETRY",
    u16(RCID_CTRL) + u16(0) + bytes([0x01, 0x02]) + u16(185))

add("V_OUT_ECHO_BODY", bytes([0xDE, 0xAD, 0xBE, 0xEF]))

# ============================================================= inbound =======

add("V_IN_CONN_RSP_CTRL",
    sig(0x03, 0x01, u16(RCID_CTRL) + u16(LCID_CTRL) + u16(0x0000) + u16(0)))
add("V_IN_CONN_RSP_INTR",
    sig(0x03, 0x02, u16(RCID_INTR) + u16(LCID_INTR) + u16(0x0000) + u16(0)))
# Pending must not be treated as either success or failure.
add("V_IN_CONN_RSP_PENDING",
    sig(0x03, 0x01, u16(RCID_CTRL) + u16(LCID_CTRL) + u16(0x0001) + u16(0)))
# Security block.
add("V_IN_CONN_RSP_REFUSED",
    sig(0x03, 0x01, u16(0x0000) + u16(LCID_CTRL) + u16(0x0003) + u16(0)))

# Their Configuration Request, naming our cid, asking for a 1024 byte MTU.
add("V_IN_CONFIG_REQ_CTRL",
    sig(0x04, ID_CFG_REQ_CTRL,
        u16(LCID_CTRL) + u16(0) + bytes([0x01, 0x02]) + u16(1024)))
add("V_IN_CONFIG_REQ_INTR",
    sig(0x04, ID_CFG_REQ_INTR,
        u16(LCID_INTR) + u16(0) + bytes([0x01, 0x02]) + u16(1024)))
# Same, but asking for retransmission mode, which we do not have.
add("V_IN_CONFIG_REQ_RETRANSMIT",
    sig(0x04, ID_UNKNOWN_OPT, u16(LCID_CTRL) + u16(0) + RETRANSMIT_OPT))
# A hint (top bit set) for something unknown must be ignored, not rejected.
add("V_IN_CONFIG_REQ_HINT",
    sig(0x04, ID_CFG_REQ_CTRL,
        u16(LCID_CTRL) + u16(0) + bytes([0x83, 0x02]) + u16(0)))
# For a channel that does not exist.
add("V_IN_CONFIG_REQ_BAD_CID",
    sig(0x04, ID_BAD_CID, u16(0x0099) + u16(0)))

add("V_IN_CONFIG_RSP_CTRL",
    sig(0x05, 0x02, u16(LCID_CTRL) + u16(0) + u16(0x0000)))
add("V_IN_CONFIG_RSP_INTR",
    sig(0x05, 0x04, u16(LCID_INTR) + u16(0) + u16(0x0000)))
# Refused with a smaller MTU offered.
add("V_IN_CONFIG_RSP_SMALLER_MTU",
    sig(0x05, 0x02,
        u16(LCID_CTRL) + u16(0) + u16(0x0001) + bytes([0x01, 0x02]) + u16(185)))
# Flatly rejected.
add("V_IN_CONFIG_RSP_REJECTED",
    sig(0x05, 0x02, u16(LCID_CTRL) + u16(0) + u16(0x0002)))

# The DualSense opening the channel itself, which is what happens on a
# reconnect: Connection Request PSM(2) Source CID(2).
add("V_IN_CONN_REQ_CTRL",
    sig(0x02, ID_CONN_REQ_IN, u16(PSM_CTRL) + u16(RCID_CTRL)))
add("V_IN_CONN_REQ_BAD_PSM",
    sig(0x02, ID_BAD_PSM, u16(0x0015) + u16(RCID_CTRL)))

add("V_IN_DISCONN_REQ",
    sig(0x06, ID_DISCONN_REQ, u16(LCID_CTRL) + u16(RCID_CTRL)))
add("V_IN_DISCONN_RSP",
    sig(0x07, 0x05, u16(RCID_CTRL) + u16(LCID_CTRL)))

add("V_IN_ECHO_REQ", sig(0x08, ID_ECHO, bytes([0xDE, 0xAD, 0xBE, 0xEF])))
add("V_IN_INFO_REQ", sig(0x0A, ID_INFO, u16(0x0002)))   # extended features
add("V_IN_UNKNOWN_CODE", sig(0x7F, ID_UNKNOWN_CODE, b""))

# Two commands in one signalling packet. A peer is allowed to do this and a
# parser that reads only the first one silently loses the second.
add("V_IN_TWO_COMMANDS",
    sig(0x08, ID_ECHO, bytes([0xDE, 0xAD, 0xBE, 0xEF])) +
    sig(0x0A, ID_INFO, u16(0x0002)))

# A DualSense input report as it arrives on the interrupt channel.
report = bytes([0xA1, 0x31]) + bytes((i * 3 + 1) & 0xFF for i in range(77))
add("V_HID_REPORT", report)

# ------------------------------------------------------------------ write ---

lines = ["/* generated by gen_l2cap_vectors.py - do not edit */\n"]
for nm, data in vectors:
    lines.append("static const uint8_t %s[] = {\n" % nm)
    for i in range(0, len(data), 12):
        lines.append("  " + " ".join("0x%02X," % b for b in data[i:i + 12]) +
                     "\n")
    lines.append("};\n")
for nm, val in [("V_LCID_CTRL", LCID_CTRL), ("V_LCID_INTR", LCID_INTR),
                ("V_RCID_CTRL", RCID_CTRL), ("V_RCID_INTR", RCID_INTR),
                ("V_MTU", MTU),
                ("V_ID_CFG_REQ_CTRL", ID_CFG_REQ_CTRL),
                ("V_ID_CFG_REQ_INTR", ID_CFG_REQ_INTR),
                ("V_ID_CONN_REQ_IN", ID_CONN_REQ_IN),
                ("V_ID_DISCONN_REQ", ID_DISCONN_REQ),
                ("V_ID_ECHO", ID_ECHO), ("V_ID_INFO", ID_INFO),
                ("V_ID_BAD_CID", ID_BAD_CID),
                ("V_ID_UNKNOWN_CODE", ID_UNKNOWN_CODE),
                ("V_ID_BAD_PSM", ID_BAD_PSM),
                ("V_ID_UNKNOWN_OPT", ID_UNKNOWN_OPT)]:
    lines.append("#define %s 0x%04X\n" % (nm, val))

io.open(OUT, "w", encoding="utf-8", newline="").write("".join(lines))
print("wrote %s, %d vectors" % (OUT, len(vectors)))

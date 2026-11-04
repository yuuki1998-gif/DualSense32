/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_hci.h — HCI packet layer, independent of any transport.
 *
 * Same idea as ds_protocol.h one level down: this knows how a Bluetooth host
 * controller interface packet is laid out and nothing else. No USB, no UART,
 * no ESP-IDF, no state machine. Build a command, parse an event, split and
 * reassemble ACL data. Who carries the bytes is somebody else's problem.
 *
 * Source: Bluetooth Core Specification, Vol 4 Part E (HCI Functional
 * Specification) and Vol 4 Part B (HCI USB Transport Layer).
 *
 *
 * THE ONE TRAP: H2 CARRIES NO PACKET-TYPE BYTE
 *
 * Over UART (H4) every packet is prefixed with a type indicator — 0x01 for a
 * command, 0x02 ACL, 0x04 event. Over USB (H2) that byte does not exist,
 * because the endpoint already says what the packet is:
 *
 *     HCI command   control transfer on endpoint 0  (see DS_HCI_USB_* below)
 *     HCI event     interrupt IN
 *     ACL data      bulk IN / bulk OUT
 *     SCO audio     isochronous          <- not needed for a DualSense
 *
 * Everything in this file therefore builds and parses BARE packets, with no
 * type byte. Prepending one to a USB transfer is a silent failure: the
 * controller reads your 0x01 as the low byte of an opcode.
 */
#ifndef DS_HCI_H
#define DS_HCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*                        U S B   T R A N S P O R T                           */
/* ========================================================================== */

/* A command goes out as the data stage of a class control transfer:
 *   bmRequestType 0x20  (host to device, class, recipient device)
 *   bRequest      0x00
 *   wValue        0x0000
 *   wIndex        the interface number, which is 0 on every standard dongle
 *   wLength       the command length
 * There is no reply on the control pipe; the answer arrives as a Command
 * Complete or Command Status event on the interrupt IN endpoint. */
#define DS_HCI_USB_BMREQUESTTYPE  0x20
#define DS_HCI_USB_BREQUEST       0x00
#define DS_HCI_USB_WVALUE         0x0000
#define DS_HCI_USB_WINDEX         0x0000

/* USB descriptor triple of a standard Bluetooth dongle. A device matching all
 * three speaks plain HCI and needs no vendor-specific transport. */
#define DS_HCI_USB_CLASS          0xE0  /* Wireless Controller   */
#define DS_HCI_USB_SUBCLASS       0x01  /* RF Controller         */
#define DS_HCI_USB_PROTOCOL       0x01  /* Bluetooth Programming */

/* ========================================================================== */
/*                              O P C O D E S                                 */
/* ========================================================================== */

/* An opcode is a 6-bit group field and a 10-bit command field. */
#define DS_HCI_OPCODE(ogf, ocf)  ((uint16_t)(((uint16_t)(ogf) << 10) | (ocf)))
#define DS_HCI_OGF(opcode)       ((uint8_t)((opcode) >> 10))
#define DS_HCI_OCF(opcode)       ((uint16_t)((opcode) & 0x03FF))

#define DS_HCI_OGF_LINK_CONTROL   0x01
#define DS_HCI_OGF_LINK_POLICY    0x02
#define DS_HCI_OGF_CONTROLLER     0x03
#define DS_HCI_OGF_INFORMATIONAL  0x04

/* Link control (OGF 0x01) */
#define DS_HCI_INQUIRY                     0x0401
#define DS_HCI_INQUIRY_CANCEL              0x0402
#define DS_HCI_CREATE_CONNECTION           0x0405
#define DS_HCI_DISCONNECT                  0x0406
#define DS_HCI_ACCEPT_CONNECTION_REQUEST   0x0409
#define DS_HCI_REJECT_CONNECTION_REQUEST   0x040A
#define DS_HCI_LINK_KEY_REQUEST_REPLY      0x040B
#define DS_HCI_LINK_KEY_REQUEST_NEG_REPLY  0x040C
#define DS_HCI_AUTHENTICATION_REQUESTED    0x0411
#define DS_HCI_SET_CONNECTION_ENCRYPTION   0x0413
#define DS_HCI_REMOTE_NAME_REQUEST         0x0419
#define DS_HCI_IO_CAPABILITY_REQUEST_REPLY 0x042B
#define DS_HCI_USER_CONFIRMATION_REPLY     0x042C
#define DS_HCI_USER_CONFIRMATION_NEG_REPLY 0x042D

/* Link control (OGF 0x01), asked once a connection exists. */
#define DS_HCI_READ_REMOTE_FEATURES        0x041B
#define DS_HCI_READ_REMOTE_EXT_FEATURES    0x041C

/* Status parameters (OGF 0x05) */
#define DS_HCI_READ_ENCRYPTION_KEY_SIZE    0x1408

/* Link policy (OGF 0x02) */
#define DS_HCI_SWITCH_ROLE                 0x080B
#define DS_HCI_WRITE_DEFAULT_LINK_POLICY   0x080F

/* Controller and baseband, the rest of what a full host configures. */
#define DS_HCI_SET_EVENT_MASK_PAGE_2       0x0C63
#define DS_HCI_WRITE_SECURE_CONN_HOST      0x0C7A
#define DS_HCI_WRITE_INQUIRY_MODE          0x0C45
#define DS_HCI_WRITE_PAGE_TIMEOUT          0x0C18
#define DS_HCI_WRITE_LINK_SUPERVISION_TO   0x0C37

/* Link supervision timeout in 0.625 ms slots. 0x7D00 is 20 s, the Bluetooth
 * default and what the pairing path already runs at. A DualSense that pages
 * us picks 5 s instead, which it does not survive. */
#define DS_HCI_SUPERVISION_TIMEOUT_DEFAULT 0x7D00

/* Informational reads a host is expected to make at start-up. */
#define DS_HCI_READ_LOCAL_COMMANDS         0x1002
#define DS_HCI_READ_LOCAL_FEATURES         0x1003

/* Page timeout in 0.625 ms slots. 0x2000 is 5.12 s, the usual choice: long
 * enough for a pad that is still waking its radio, short enough that a failed
 * page does not hold everything else up. */
#define DS_HCI_PAGE_TIMEOUT_DEFAULT        0x2000

/* Page scan (OGF 0x03). The controller's own defaults listen for only one of
 * the two ways a peer may page, for 11.25 ms out of every 1.28 s -- so a
 * bonded pad calling in on PS often has to try twice, a second or more late.
 * These are the values Linux calls "fast connectable": both page forms,
 * every 160 ms. */
#define DS_HCI_WRITE_PAGE_SCAN_ACTIVITY    0x0C1C
#define DS_HCI_WRITE_PAGE_SCAN_TYPE        0x0C47
#define DS_HCI_PAGE_SCAN_INTERVAL_DEFAULT  0x0100   /* 160 ms   */
#define DS_HCI_PAGE_SCAN_WINDOW_DEFAULT    0x0012   /* 11.25 ms */
#define DS_HCI_PAGE_SCAN_TYPE_INTERLACED   0x01

/* Controller and baseband (OGF 0x03) */
#define DS_HCI_SET_EVENT_MASK              0x0C01
#define DS_HCI_RESET                       0x0C03
#define DS_HCI_WRITE_LOCAL_NAME            0x0C13
#define DS_HCI_WRITE_SCAN_ENABLE           0x0C1A
#define DS_HCI_WRITE_AUTHENTICATION_ENABLE 0x0C20
#define DS_HCI_WRITE_CLASS_OF_DEVICE       0x0C24
#define DS_HCI_HOST_BUFFER_SIZE            0x0C33
#define DS_HCI_WRITE_INQUIRY_MODE          0x0C45
#define DS_HCI_WRITE_SIMPLE_PAIRING_MODE   0x0C56

/* Informational (OGF 0x04) */
#define DS_HCI_READ_LOCAL_VERSION          0x1001
#define DS_HCI_READ_BUFFER_SIZE            0x1005
#define DS_HCI_READ_BD_ADDR                0x1009

/* ========================================================================== */
/*                            E V E N T   C O D E S                           */
/* ========================================================================== */

#define DS_HCI_EVT_INQUIRY_COMPLETE          0x01
#define DS_HCI_EVT_INQUIRY_RESULT            0x02
#define DS_HCI_EVT_CONNECTION_COMPLETE       0x03
#define DS_HCI_EVT_CONNECTION_REQUEST        0x04
#define DS_HCI_EVT_DISCONNECTION_COMPLETE    0x05
#define DS_HCI_EVT_AUTHENTICATION_COMPLETE   0x06
#define DS_HCI_EVT_REMOTE_NAME_COMPLETE      0x07
#define DS_HCI_EVT_ENCRYPTION_CHANGE         0x08
#define DS_HCI_EVT_REMOTE_FEATURES_COMPLETE  0x0B
#define DS_HCI_EVT_QOS_SETUP_COMPLETE        0x0D
#define DS_HCI_EVT_REMOTE_EXT_FEATURES       0x23
#define DS_HCI_EVT_COMMAND_COMPLETE          0x0E
#define DS_HCI_EVT_COMMAND_STATUS            0x0F
#define DS_HCI_EVT_NUM_COMPLETED_PACKETS     0x13
#define DS_HCI_EVT_ROLE_CHANGE               0x12
#define DS_HCI_EVT_MODE_CHANGE               0x14
#define DS_HCI_EVT_LINK_KEY_REQUEST          0x17
#define DS_HCI_EVT_LINK_KEY_NOTIFICATION     0x18
#define DS_HCI_EVT_MAX_SLOTS_CHANGE          0x1B
#define DS_HCI_EVT_INQUIRY_RESULT_RSSI       0x22
#define DS_HCI_EVT_EXTENDED_INQUIRY_RESULT   0x2F
#define DS_HCI_EVT_IO_CAPABILITY_REQUEST     0x31
#define DS_HCI_EVT_IO_CAPABILITY_RESPONSE    0x32
#define DS_HCI_EVT_USER_CONFIRMATION_REQUEST 0x33
#define DS_HCI_EVT_SIMPLE_PAIRING_COMPLETE   0x36

/* ========================================================================== */
/*                           C O N S T A N T S                                */
/* ========================================================================== */

#define DS_HCI_SUCCESS            0x00
#define DS_HCI_BD_ADDR_LEN        6
#define DS_HCI_LINK_KEY_LEN       16
#define DS_HCI_LOCAL_NAME_LEN     248  /* fixed width, zero padded */

#define DS_HCI_CMD_HDR_LEN        3    /* opcode 2 + parameter length 1 */
#define DS_HCI_EVT_HDR_LEN        2    /* code 1   + parameter length 1 */
#define DS_HCI_ACL_HDR_LEN        4    /* handle+flags 2 + length 2     */
#define DS_HCI_CMD_MAX_PARAMS     255
#define DS_HCI_CMD_MAX_LEN        (DS_HCI_CMD_HDR_LEN + DS_HCI_CMD_MAX_PARAMS)

/* General Inquiry Access Code, the LAP every discoverable device answers. */
#define DS_HCI_LAP_GIAC           0x9E8B33UL

/* Packet types for Create Connection: the six basic DM/DH packets. Naming a
 * 2-Mbit or 3-Mbit type in this field actually DISABLES it, which is why the
 * value reads inverted compared to what you would guess. */
#define DS_HCI_PKT_TYPE_DEFAULT   0xCC18

/* Scan enable */
#define DS_HCI_SCAN_NONE          0x00
#define DS_HCI_SCAN_INQUIRY       0x01
#define DS_HCI_SCAN_PAGE          0x02
#define DS_HCI_SCAN_BOTH          0x03

/* IO capability. A dongle driving a DualSense has no display and no keypad,
 * so NO_INPUT_OUTPUT is the honest answer and gets Just Works pairing. */
#define DS_HCI_IO_DISPLAY_ONLY    0x00
#define DS_HCI_IO_DISPLAY_YES_NO  0x01
#define DS_HCI_IO_KEYBOARD_ONLY   0x02
#define DS_HCI_IO_NO_INPUT_OUTPUT 0x03

/* Authentication requirements */
#define DS_HCI_AUTH_NO_MITM_NO_BONDING 0x00
#define DS_HCI_AUTH_MITM_NO_BONDING    0x01
#define DS_HCI_AUTH_NO_MITM_DEDICATED  0x02
#define DS_HCI_AUTH_MITM_DEDICATED     0x03
#define DS_HCI_AUTH_NO_MITM_GENERAL    0x04
#define DS_HCI_AUTH_MITM_GENERAL       0x05

/* Link policy settings. A controller comes out of reset with all of these
 * off, which means it refuses every low-power and role-switch request the
 * peer makes. A gamepad that wants to doze between reports and is told no
 * each time is not a hypothetical: it is what a DualSense does. */
#define DS_HCI_POLICY_ROLE_SWITCH 0x0001
#define DS_HCI_POLICY_HOLD        0x0002
#define DS_HCI_POLICY_SNIFF       0x0004
#define DS_HCI_POLICY_DEFAULT     (DS_HCI_POLICY_ROLE_SWITCH | \
                                    DS_HCI_POLICY_SNIFF)

/* ACL packet boundary flag, carried in the top of the handle word. */
#define DS_HCI_PB_FIRST_NON_FLUSH 0x00
#define DS_HCI_PB_CONTINUING      0x01
#define DS_HCI_PB_FIRST_FLUSH     0x02
#define DS_HCI_PB_COMPLETE        0x03

/* ========================================================================== */
/*                             C O M M A N D S                                */
/* ========================================================================== */

/* Every builder writes a bare command packet and returns its length, or 0 if
 * the buffer is too small or a parameter is out of range. Zero is never a
 * valid length, so one check covers both.
 *
 * ds_hci_cmd is the generic form; params may be NULL when plen is 0. */
uint16_t ds_hci_cmd(uint8_t *buf, uint16_t buflen, uint16_t opcode,
                     const uint8_t *params, uint8_t plen);

/* Commands whose parameters are worth packing for you. The rest are either
 * parameterless or a single byte, and go through ds_hci_cmd directly. */

uint16_t ds_hci_cmd_reset(uint8_t *buf, uint16_t buflen);
uint16_t ds_hci_cmd_set_event_mask(uint8_t *buf, uint16_t buflen, uint64_t mask);
uint16_t ds_hci_cmd_write_scan_enable(uint8_t *buf, uint16_t buflen, uint8_t scan);
uint16_t ds_hci_cmd_write_local_name(uint8_t *buf, uint16_t buflen, const char *name);
uint16_t ds_hci_cmd_write_class_of_device(uint8_t *buf, uint16_t buflen, uint32_t cod);
uint16_t ds_hci_cmd_write_simple_pairing_mode(uint8_t *buf, uint16_t buflen, bool on);

/* lap is normally DS_HCI_LAP_GIAC. length is in 1.28 s units (1-48) and
 * max_responses 0 means unlimited. */
uint16_t ds_hci_cmd_inquiry(uint8_t *buf, uint16_t buflen, uint32_t lap,
                             uint8_t length, uint8_t max_responses);

/* addr is in wire order, which is little endian — the same order every HCI
 * event hands it back in. A BD_ADDR copied out of an Inquiry Result can be
 * passed straight here without reversing it. */
uint16_t ds_hci_cmd_create_connection(uint8_t *buf, uint16_t buflen,
                                       const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                       uint16_t packet_types,
                                       uint8_t page_scan_repetition_mode,
                                       uint16_t clock_offset,
                                       bool allow_role_switch);

/* Any command whose whole payload is a connection handle. */
uint16_t ds_hci_cmd_handle(uint8_t *buf, uint16_t buflen, uint16_t opcode,
                            uint16_t handle);
uint16_t ds_hci_cmd_read_remote_ext_features(uint8_t *buf, uint16_t buflen,
                                              uint16_t handle, uint8_t page);

uint16_t ds_hci_cmd_accept_connection(uint8_t *buf, uint16_t buflen,
                                       const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                       uint8_t role);

uint16_t ds_hci_cmd_disconnect(uint8_t *buf, uint16_t buflen, uint16_t handle,
                                uint8_t reason);

uint16_t ds_hci_cmd_link_key_reply(uint8_t *buf, uint16_t buflen,
                                    const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                    const uint8_t key[DS_HCI_LINK_KEY_LEN]);

uint16_t ds_hci_cmd_link_key_neg_reply(uint8_t *buf, uint16_t buflen,
                                        const uint8_t addr[DS_HCI_BD_ADDR_LEN]);

uint16_t ds_hci_cmd_io_capability_reply(uint8_t *buf, uint16_t buflen,
                                         const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                         uint8_t io_capability,
                                         uint8_t oob_present,
                                         uint8_t auth_requirements);

uint16_t ds_hci_cmd_user_confirmation_reply(uint8_t *buf, uint16_t buflen,
                                             const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                             bool accept);

uint16_t ds_hci_cmd_authentication_requested(uint8_t *buf, uint16_t buflen,
                                              uint16_t handle);

uint16_t ds_hci_cmd_set_connection_encryption(uint8_t *buf, uint16_t buflen,
                                               uint16_t handle, bool enable);

/* ========================================================================== */
/*                               E V E N T S                                  */
/* ========================================================================== */

/* params points into the caller's buffer and stays valid only as long as that
 * buffer does. Nothing in this layer copies. */
typedef struct {
  uint8_t code;
  uint8_t plen;
  const uint8_t *params;
} ds_hci_event_t;

bool ds_hci_parse_event(const uint8_t *pkt, uint16_t len, ds_hci_event_t *ev);

typedef struct {
  uint8_t num_cmd_packets;  /* how many more commands the controller will take */
  uint16_t opcode;
  uint8_t status;           /* first return parameter; 0xFF if there were none */
  const uint8_t *ret;       /* return parameters INCLUDING the status byte     */
  uint8_t ret_len;
} ds_hci_cmd_complete_t;

bool ds_hci_parse_cmd_complete(const ds_hci_event_t *ev,
                                ds_hci_cmd_complete_t *out);

typedef struct {
  uint8_t status;
  uint8_t num_cmd_packets;
  uint16_t opcode;
} ds_hci_cmd_status_t;

bool ds_hci_parse_cmd_status(const ds_hci_event_t *ev,
                              ds_hci_cmd_status_t *out);

typedef struct {
  uint8_t status;
  uint16_t handle;
  uint8_t addr[DS_HCI_BD_ADDR_LEN];
  uint8_t link_type;        /* 0x01 = ACL */
  uint8_t encryption;
} ds_hci_conn_complete_t;

bool ds_hci_parse_conn_complete(const ds_hci_event_t *ev,
                                 ds_hci_conn_complete_t *out);

typedef struct {
  uint8_t addr[DS_HCI_BD_ADDR_LEN];
  uint32_t class_of_device;
  uint8_t link_type;
} ds_hci_conn_request_t;

bool ds_hci_parse_conn_request(const ds_hci_event_t *ev,
                                ds_hci_conn_request_t *out);

typedef struct {
  uint8_t status;
  uint16_t handle;
  uint8_t reason;
} ds_hci_disconn_complete_t;

bool ds_hci_parse_disconn_complete(const ds_hci_event_t *ev,
                                    ds_hci_disconn_complete_t *out);

/* One entry of an Inquiry Result, an Inquiry Result with RSSI or an Extended
 * Inquiry Result, normalised. Do not assume the three share a layout: adding
 * RSSI dropped one of the two reserved bytes, so the class of device and
 * clock offset sit one byte EARLIER in 0x22 and 0x2F than in 0x02. Only the
 * address and the page scan repetition mode are at a common offset.
 *
 * index selects the response, 0 upwards. Returns false once index runs past
 * the count, which makes it a loop condition. */
typedef struct {
  uint8_t addr[DS_HCI_BD_ADDR_LEN];
  uint8_t page_scan_repetition_mode;
  uint32_t class_of_device;
  uint16_t clock_offset;
  int8_t rssi;
  bool has_rssi;
} ds_hci_inquiry_result_t;

uint8_t ds_hci_inquiry_result_count(const ds_hci_event_t *ev);
bool ds_hci_parse_inquiry_result(const ds_hci_event_t *ev, uint8_t index,
                                  ds_hci_inquiry_result_t *out);

/* Events that carry a bare BD_ADDR and nothing else: Link Key Request and
 * IO Capability Request. */
bool ds_hci_parse_addr_event(const ds_hci_event_t *ev,
                              uint8_t addr[DS_HCI_BD_ADDR_LEN]);

typedef struct {
  uint8_t addr[DS_HCI_BD_ADDR_LEN];
  uint8_t key[DS_HCI_LINK_KEY_LEN];
  uint8_t key_type;
} ds_hci_link_key_notification_t;

bool ds_hci_parse_link_key_notification(const ds_hci_event_t *ev,
                                         ds_hci_link_key_notification_t *out);

typedef struct {
  uint8_t addr[DS_HCI_BD_ADDR_LEN];
  uint32_t numeric_value;   /* the six digits a display would show */
} ds_hci_user_confirmation_t;

bool ds_hci_parse_user_confirmation(const ds_hci_event_t *ev,
                                     ds_hci_user_confirmation_t *out);

/* ========================================================================== */
/*                             A C L   D A T A                                */
/* ========================================================================== */

typedef struct {
  uint16_t handle;
  uint8_t pb;               /* packet boundary flag, DS_HCI_PB_*   */
  uint8_t bc;               /* broadcast flag, 0 for point to point */
  const uint8_t *data;      /* points into the caller's buffer      */
  uint16_t len;
} ds_hci_acl_t;

bool ds_hci_parse_acl(const uint8_t *pkt, uint16_t pktlen, ds_hci_acl_t *out);

/* Writes the 4-byte ACL header only; the payload is the caller's to append.
 * Returns DS_HCI_ACL_HDR_LEN, or 0 if the buffer is too small. */
uint16_t ds_hci_acl_header(uint8_t *buf, uint16_t buflen, uint16_t handle,
                            uint8_t pb, uint16_t payload_len);

/* --- reassembly ----------------------------------------------------------
 *
 * The controller splits an L2CAP packet across as many ACL fragments as its
 * buffer size demands. This reassembler tracks one handle, which is all a
 * single DualSense needs.
 *
 * The total length is taken from the L2CAP header at the front of the first
 * fragment: two bytes of payload length, then the two byte channel id. */

#ifndef DS_HCI_REASM_MAX
#define DS_HCI_REASM_MAX 1024
#endif

typedef struct {
  uint8_t buf[DS_HCI_REASM_MAX];
  uint16_t len;             /* bytes held so far        */
  uint16_t want;            /* total expected, 0 = idle */
  uint16_t handle;
} ds_hci_reasm_t;

typedef enum {
  DS_HCI_REASM_NEED_MORE = 0,  /* fragment stored, packet still incomplete */
  DS_HCI_REASM_DONE = 1,       /* a whole L2CAP packet is available        */
  DS_HCI_REASM_ERROR = 2,      /* malformed or oversized; state was reset  */
} ds_hci_reasm_result_t;

void ds_hci_reasm_reset(ds_hci_reasm_t *r);

/* On DS_HCI_REASM_DONE, *out points at the complete L2CAP packet starting at
 * its length field and *out_len is its size. The pointer stays valid until
 * the next push. */
ds_hci_reasm_result_t ds_hci_reasm_push(ds_hci_reasm_t *r,
                                          const ds_hci_acl_t *acl,
                                          const uint8_t **out,
                                          uint16_t *out_len);

/* ========================================================================== */
/*                              H E L P E R S                                 */
/* ========================================================================== */

/* BD_ADDR is little endian everywhere in HCI but is written big endian
 * everywhere humans are involved, which is a reliable source of bugs.
 * str needs 18 bytes. */
void ds_hci_addr_to_str(const uint8_t addr[DS_HCI_BD_ADDR_LEN], char *str);

/* Parses "AA:BB:CC:DD:EE:FF" into wire order. Returns false on anything else. */
bool ds_hci_str_to_addr(const char *str, uint8_t addr[DS_HCI_BD_ADDR_LEN]);

/* Human readable name for a status or reason code, or "unknown". The reason
 * carried by Disconnection Complete is the single most useful byte when a link
 * ends by itself, so it is worth printing rather than swallowing. */
const char *ds_hci_error_name(uint8_t code);

/* Human readable event name, or "unknown" — for logs, never for control flow. */
const char *ds_hci_event_name(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* DS_HCI_H */

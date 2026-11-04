#include <ds_hci.h>

#include "vectors.h"

/*
 * HciPackets — validates the HCI packet layer.
 *
 * No dongle, no USB, no controller. This only checks that ds_hci turns
 * parameters into the exact bytes the Bluetooth spec calls for, and turns
 * spec-shaped events back into the right fields. The vectors in vectors.h are
 * written out from the Core Specification field lists by gen_hci_vectors.py,
 * independently of the implementation, so a transcription slip fails here
 * rather than being baked into both sides.
 *
 * Runs on any ESP32. Repeats every few seconds because the native USB CDC on
 * an S3 misses whatever is printed before the host opens the port.
 */

static int passCount = 0, failCount = 0;

static void check(const char* name, bool ok) {
  if (ok) { passCount++; return; }
  failCount++;
  Serial.printf("  FAIL  %s\n", name);
}

static void checkEq(const char* name, uint32_t got, uint32_t want) {
  if (got == want) { passCount++; return; }
  failCount++;
  Serial.printf("  FAIL  %s  got 0x%lX want 0x%lX\n", name,
                (unsigned long)got, (unsigned long)want);
}

static void checkBytes(const char* name, const uint8_t* got, const uint8_t* want,
                       size_t n) {
  if (memcmp(got, want, n) == 0) { passCount++; return; }
  failCount++;
  Serial.printf("  FAIL  %s\n   got  ", name);
  for (size_t i = 0; i < n; i++) Serial.printf("%02X ", got[i]);
  Serial.print("\n   want ");
  for (size_t i = 0; i < n; i++) Serial.printf("%02X ", want[i]);
  Serial.println();
}

/* Builds into a scratch buffer and compares against a vector, length first. */
static uint8_t buf[DS_HCI_CMD_MAX_LEN];

static void checkCmd(const char* name, uint16_t n, const uint8_t* want,
                     size_t wantLen) {
  if (n != wantLen) {
    failCount++;
    Serial.printf("  FAIL  %s  length %u want %u\n", name, n,
                  (unsigned)wantLen);
    return;
  }
  checkBytes(name, buf, want, wantLen);
}

/* ---------------------------------------------------------------- opcodes */

static void testOpcodes() {
  Serial.println("opcodes");
  /* OGF in the top 6 bits, OCF in the bottom 10. */
  checkEq("opcode(3,3) = reset", DS_HCI_OPCODE(0x03, 0x0003), DS_HCI_RESET);
  checkEq("opcode(1,5) = create conn", DS_HCI_OPCODE(0x01, 0x0005),
          DS_HCI_CREATE_CONNECTION);
  checkEq("opcode(4,1) = local version", DS_HCI_OPCODE(0x04, 0x0001),
          DS_HCI_READ_LOCAL_VERSION);
  checkEq("ogf(reset)", DS_HCI_OGF(DS_HCI_RESET), 0x03);
  checkEq("ocf(reset)", DS_HCI_OCF(DS_HCI_RESET), 0x0003);
  checkEq("ogf(io cap reply)", DS_HCI_OGF(DS_HCI_IO_CAPABILITY_REQUEST_REPLY),
          0x01);
  checkEq("ocf(io cap reply)", DS_HCI_OCF(DS_HCI_IO_CAPABILITY_REQUEST_REPLY),
          0x02B);
}

/* --------------------------------------------------------------- commands */

static void testCommands() {
  Serial.println("commands");

  checkCmd("reset", ds_hci_cmd_reset(buf, sizeof(buf)), V_CMD_RESET,
           sizeof(V_CMD_RESET));

  checkCmd("read local version",
           ds_hci_cmd(buf, sizeof(buf), DS_HCI_READ_LOCAL_VERSION, NULL, 0),
           V_CMD_READ_LOCAL_VERSION, sizeof(V_CMD_READ_LOCAL_VERSION));

  checkCmd("write scan enable both",
           ds_hci_cmd_write_scan_enable(buf, sizeof(buf), DS_HCI_SCAN_BOTH),
           V_CMD_SCAN_BOTH, sizeof(V_CMD_SCAN_BOTH));

  checkCmd("set event mask all ones",
           ds_hci_cmd_set_event_mask(buf, sizeof(buf), 0xFFFFFFFFFFFFFFFFULL),
           V_CMD_EVENT_MASK_ALL, sizeof(V_CMD_EVENT_MASK_ALL));

  checkCmd("set event mask spec default",
           ds_hci_cmd_set_event_mask(buf, sizeof(buf), 0x00001FFFFFFFFFFFULL),
           V_CMD_EVENT_MASK_DEFAULT, sizeof(V_CMD_EVENT_MASK_DEFAULT));

  checkCmd("write class of device",
           ds_hci_cmd_write_class_of_device(buf, sizeof(buf), 0x002508),
           V_CMD_COD, sizeof(V_CMD_COD));

  checkCmd("write simple pairing mode on",
           ds_hci_cmd_write_simple_pairing_mode(buf, sizeof(buf), true),
           V_CMD_SSP_ON, sizeof(V_CMD_SSP_ON));

  checkCmd("inquiry giac 8 unlimited",
           ds_hci_cmd_inquiry(buf, sizeof(buf), DS_HCI_LAP_GIAC, 8, 0),
           V_CMD_INQUIRY, sizeof(V_CMD_INQUIRY));

  checkCmd("create connection",
           ds_hci_cmd_create_connection(buf, sizeof(buf), V_ADDR,
                                         DS_HCI_PKT_TYPE_DEFAULT, 0x01, 0x0000,
                                         true),
           V_CMD_CREATE_CONN, sizeof(V_CMD_CREATE_CONN));

  checkCmd("disconnect",
           ds_hci_cmd_disconnect(buf, sizeof(buf), 0x000C, 0x13),
           V_CMD_DISCONNECT, sizeof(V_CMD_DISCONNECT));

  checkCmd("io capability reply",
           ds_hci_cmd_io_capability_reply(buf, sizeof(buf), V_ADDR,
                                           DS_HCI_IO_NO_INPUT_OUTPUT, 0x00,
                                           DS_HCI_AUTH_NO_MITM_GENERAL),
           V_CMD_IOCAP_REPLY, sizeof(V_CMD_IOCAP_REPLY));

  checkCmd("link key reply",
           ds_hci_cmd_link_key_reply(buf, sizeof(buf), V_ADDR, V_LINK_KEY),
           V_CMD_LINK_KEY_REPLY, sizeof(V_CMD_LINK_KEY_REPLY));

  checkCmd("link key negative reply",
           ds_hci_cmd_link_key_neg_reply(buf, sizeof(buf), V_ADDR),
           V_CMD_LINK_KEY_NEG_REPLY, sizeof(V_CMD_LINK_KEY_NEG_REPLY));

  checkCmd("user confirmation accept",
           ds_hci_cmd_user_confirmation_reply(buf, sizeof(buf), V_ADDR, true),
           V_CMD_USER_CONF_ACCEPT, sizeof(V_CMD_USER_CONF_ACCEPT));

  checkCmd("user confirmation reject",
           ds_hci_cmd_user_confirmation_reply(buf, sizeof(buf), V_ADDR, false),
           V_CMD_USER_CONF_REJECT, sizeof(V_CMD_USER_CONF_REJECT));

  checkCmd("authentication requested",
           ds_hci_cmd_authentication_requested(buf, sizeof(buf), 0x000C),
           V_CMD_AUTH_REQ, sizeof(V_CMD_AUTH_REQ));

  checkCmd("set connection encryption",
           ds_hci_cmd_set_connection_encryption(buf, sizeof(buf), 0x000C, true),
           V_CMD_SET_ENCRYPTION, sizeof(V_CMD_SET_ENCRYPTION));

  /* The name field is a fixed 248 bytes however short the name is. */
  checkCmd("write local name is 248 padded",
           ds_hci_cmd_write_local_name(buf, sizeof(buf), "DualSense32"),
           V_CMD_LOCAL_NAME, sizeof(V_CMD_LOCAL_NAME));
}

static void testCommandRejects() {
  Serial.println("command rejects");

  check("buffer too small", ds_hci_cmd_reset(buf, 2) == 0);
  check("params without a pointer",
        ds_hci_cmd(buf, sizeof(buf), DS_HCI_RESET, NULL, 4) == 0);
  check("scan enable 4 rejected",
        ds_hci_cmd_write_scan_enable(buf, sizeof(buf), 4) == 0);
  check("inquiry length 0 rejected",
        ds_hci_cmd_inquiry(buf, sizeof(buf), DS_HCI_LAP_GIAC, 0, 0) == 0);
  check("inquiry length 49 rejected",
        ds_hci_cmd_inquiry(buf, sizeof(buf), DS_HCI_LAP_GIAC, 49, 0) == 0);
  check("io capability 4 rejected",
        ds_hci_cmd_io_capability_reply(buf, sizeof(buf), V_ADDR, 4, 0, 0) == 0);
  check("auth requirements 6 rejected",
        ds_hci_cmd_io_capability_reply(buf, sizeof(buf), V_ADDR, 3, 0, 6) == 0);
  check("null address rejected",
        ds_hci_cmd_create_connection(buf, sizeof(buf), NULL, 0xCC18, 1, 0,
                                      true) == 0);
  /* 251 bytes needed; 250 must fail rather than truncate the name field. */
  check("local name into 250 bytes rejected",
        ds_hci_cmd_write_local_name(buf, 250, "DualSense32") == 0);
}

/* ----------------------------------------------------------------- events */

static void testEvents() {
  Serial.println("events");
  ds_hci_event_t ev;

  /* --- command complete, carrying the dongle's own version reply --- */
  check("parse command complete event",
        ds_hci_parse_event(V_EVT_CC_LOCAL_VERSION,
                            sizeof(V_EVT_CC_LOCAL_VERSION), &ev));
  checkEq("cc code", ev.code, DS_HCI_EVT_COMMAND_COMPLETE);
  ds_hci_cmd_complete_t cc;
  check("decode command complete", ds_hci_parse_cmd_complete(&ev, &cc));
  checkEq("cc num cmd packets", cc.num_cmd_packets, 1);
  checkEq("cc opcode", cc.opcode, DS_HCI_READ_LOCAL_VERSION);
  checkEq("cc status", cc.status, DS_HCI_SUCCESS);
  checkEq("cc return length", cc.ret_len, 9);
  checkEq("cc hci version 14", cc.ret[1], 14);
  checkEq("cc manufacturer 2279",
          (uint16_t)(cc.ret[5] | ((uint16_t)cc.ret[6] << 8)), 2279);

  /* --- command status --- */
  check("parse command status event",
        ds_hci_parse_event(V_EVT_CS_CREATE_CONN, sizeof(V_EVT_CS_CREATE_CONN),
                            &ev));
  ds_hci_cmd_status_t cs;
  check("decode command status", ds_hci_parse_cmd_status(&ev, &cs));
  checkEq("cs status", cs.status, DS_HCI_SUCCESS);
  checkEq("cs opcode", cs.opcode, DS_HCI_CREATE_CONNECTION);

  /* --- connection complete --- */
  check("parse connection complete",
        ds_hci_parse_event(V_EVT_CONN_COMPLETE, sizeof(V_EVT_CONN_COMPLETE),
                            &ev));
  ds_hci_conn_complete_t conn;
  check("decode connection complete", ds_hci_parse_conn_complete(&ev, &conn));
  checkEq("conn status", conn.status, DS_HCI_SUCCESS);
  checkEq("conn handle", conn.handle, 0x000C);
  checkBytes("conn address", conn.addr, V_ADDR, DS_HCI_BD_ADDR_LEN);
  checkEq("conn link type acl", conn.link_type, 0x01);

  /* --- connection request --- */
  check("parse connection request",
        ds_hci_parse_event(V_EVT_CONN_REQUEST, sizeof(V_EVT_CONN_REQUEST),
                            &ev));
  ds_hci_conn_request_t req;
  check("decode connection request", ds_hci_parse_conn_request(&ev, &req));
  checkBytes("request address", req.addr, V_ADDR, DS_HCI_BD_ADDR_LEN);
  checkEq("request class of device", req.class_of_device, 0x002508);

  /* --- disconnection complete --- */
  check("parse disconnection complete",
        ds_hci_parse_event(V_EVT_DISCONN, sizeof(V_EVT_DISCONN), &ev));
  ds_hci_disconn_complete_t dis;
  check("decode disconnection", ds_hci_parse_disconn_complete(&ev, &dis));
  checkEq("disconnect handle", dis.handle, 0x000C);
  checkEq("disconnect reason", dis.reason, 0x13);

  /* --- pairing events --- */
  uint8_t addr[DS_HCI_BD_ADDR_LEN];
  check("parse io capability request",
        ds_hci_parse_event(V_EVT_IOCAP_REQ, sizeof(V_EVT_IOCAP_REQ), &ev));
  check("decode io capability request", ds_hci_parse_addr_event(&ev, addr));
  checkBytes("io capability address", addr, V_ADDR, DS_HCI_BD_ADDR_LEN);

  check("parse link key request",
        ds_hci_parse_event(V_EVT_LINK_KEY_REQ, sizeof(V_EVT_LINK_KEY_REQ), &ev));
  check("decode link key request", ds_hci_parse_addr_event(&ev, addr));

  check("parse link key notification",
        ds_hci_parse_event(V_EVT_LINK_KEY_NOTIF, sizeof(V_EVT_LINK_KEY_NOTIF),
                            &ev));
  ds_hci_link_key_notification_t notif;
  check("decode link key notification",
        ds_hci_parse_link_key_notification(&ev, &notif));
  checkBytes("notified link key", notif.key, V_LINK_KEY, DS_HCI_LINK_KEY_LEN);
  checkEq("link key type", notif.key_type, 0x05);

  check("parse user confirmation request",
        ds_hci_parse_event(V_EVT_USER_CONF, sizeof(V_EVT_USER_CONF), &ev));
  ds_hci_user_confirmation_t uc;
  check("decode user confirmation", ds_hci_parse_user_confirmation(&ev, &uc));
  checkEq("confirmation numeric value", uc.numeric_value, 123456);
}

/* The three inquiry events are laid out differently. Parsing 0x22 with the
 * 0x02 offsets yields a class of device that is off by one byte, which is the
 * kind of wrong that looks plausible in a log. */
static void testInquiry() {
  Serial.println("inquiry results");
  ds_hci_event_t ev;
  ds_hci_inquiry_result_t r;

  check("parse inquiry result",
        ds_hci_parse_event(V_EVT_INQ_RESULT, sizeof(V_EVT_INQ_RESULT), &ev));
  checkEq("inquiry result count", ds_hci_inquiry_result_count(&ev), 1);
  check("decode inquiry result", ds_hci_parse_inquiry_result(&ev, 0, &r));
  checkBytes("inquiry address", r.addr, V_ADDR, DS_HCI_BD_ADDR_LEN);
  checkEq("inquiry psrm", r.page_scan_repetition_mode, 0x01);
  checkEq("inquiry class of device", r.class_of_device, 0x002508);
  checkEq("inquiry clock offset", r.clock_offset, 0x1234);
  check("inquiry result has no rssi", !r.has_rssi);
  check("index past the end rejected",
        !ds_hci_parse_inquiry_result(&ev, 1, &r));

  check("parse inquiry result with rssi",
        ds_hci_parse_event(V_EVT_INQ_RESULT_RSSI,
                            sizeof(V_EVT_INQ_RESULT_RSSI), &ev));
  check("decode inquiry result with rssi",
        ds_hci_parse_inquiry_result(&ev, 0, &r));
  checkBytes("rssi variant address", r.addr, V_ADDR, DS_HCI_BD_ADDR_LEN);
  checkEq("rssi variant class of device", r.class_of_device, 0x002508);
  checkEq("rssi variant clock offset", r.clock_offset, 0x1234);
  check("rssi variant has rssi", r.has_rssi);
  checkEq("rssi value -50", (uint32_t)(int32_t)r.rssi, (uint32_t)(int32_t)-50);

  check("parse two responses",
        ds_hci_parse_event(V_EVT_INQ_RESULT_2, sizeof(V_EVT_INQ_RESULT_2),
                            &ev));
  checkEq("two response count", ds_hci_inquiry_result_count(&ev), 2);
  check("decode second response", ds_hci_parse_inquiry_result(&ev, 1, &r));
  checkBytes("second address", r.addr, V_ADDR2, DS_HCI_BD_ADDR_LEN);
  checkEq("second class of device", r.class_of_device, 0x240404);
  checkEq("second clock offset", r.clock_offset, 0x5678);
}

static void testEventRejects() {
  Serial.println("event rejects");
  ds_hci_event_t ev;

  check("one byte is not an event", !ds_hci_parse_event(V_EVT_DISCONN, 1, &ev));
  /* Header claims 4 parameter bytes but only 2 arrived. */
  check("truncated event rejected",
        !ds_hci_parse_event(V_EVT_DISCONN, 4, &ev));

  ds_hci_parse_event(V_EVT_CS_CREATE_CONN, sizeof(V_EVT_CS_CREATE_CONN), &ev);
  ds_hci_cmd_complete_t cc;
  check("command status is not command complete",
        !ds_hci_parse_cmd_complete(&ev, &cc));

  ds_hci_parse_event(V_EVT_DISCONN, sizeof(V_EVT_DISCONN), &ev);
  ds_hci_conn_complete_t conn;
  check("disconnection is not connection complete",
        !ds_hci_parse_conn_complete(&ev, &conn));

  /* Right code, too few parameters. */
  const uint8_t shortConn[] = {0x03, 0x03, 0x00, 0x0C, 0x00};
  ds_hci_parse_event(shortConn, sizeof(shortConn), &ev);
  check("short connection complete rejected",
        !ds_hci_parse_conn_complete(&ev, &conn));

  /* A controller claiming three responses in a one response event must not
   * make the parser walk off the end. */
  const uint8_t liar[] = {0x02, 0x0F, 0x03, 0x66, 0x55, 0x44, 0x33, 0x22,
                          0x11, 0x01, 0x00, 0x00, 0x08, 0x25, 0x00, 0x34,
                          0x12};
  ds_hci_parse_event(liar, sizeof(liar), &ev);
  checkEq("over-claimed count clamped by length",
          ds_hci_inquiry_result_count(&ev), 1);
}

/* -------------------------------------------------------------------- acl */

static void testAcl() {
  Serial.println("acl");
  ds_hci_acl_t a;

  checkCmd("acl header",
           ds_hci_acl_header(buf, sizeof(buf), 0x000C, DS_HCI_PB_FIRST_FLUSH, 8),
           V_ACL_HDR, sizeof(V_ACL_HDR));

  check("parse one acl packet",
        ds_hci_parse_acl(V_ACL_ONE, sizeof(V_ACL_ONE), &a));
  checkEq("acl handle", a.handle, 0x000C);
  checkEq("acl pb flag", a.pb, DS_HCI_PB_FIRST_FLUSH);
  checkEq("acl bc flag", a.bc, 0);
  checkEq("acl payload length", a.len, sizeof(V_L2CAP_SMALL));
  checkBytes("acl payload", a.data, V_L2CAP_SMALL, sizeof(V_L2CAP_SMALL));

  check("acl shorter than its header rejected",
        !ds_hci_parse_acl(V_ACL_ONE, 3, &a));
  check("acl claiming more than it carries rejected",
        !ds_hci_parse_acl(V_ACL_ONE, 6, &a));
  check("handle above 12 bits rejected",
        ds_hci_acl_header(buf, sizeof(buf), 0x1000, 0, 8) == 0);
}

static void testReassembly() {
  Serial.println("acl reassembly");
  ds_hci_reasm_t r;
  ds_hci_acl_t a;
  const uint8_t* out = NULL;
  uint16_t outLen = 0;

  /* A packet that arrives whole in one fragment is done immediately. */
  ds_hci_reasm_reset(&r);
  ds_hci_parse_acl(V_ACL_ONE, sizeof(V_ACL_ONE), &a);
  checkEq("single fragment completes",
          ds_hci_reasm_push(&r, &a, &out, &outLen), DS_HCI_REASM_DONE);
  checkEq("single fragment length", outLen, sizeof(V_L2CAP_SMALL));
  checkBytes("single fragment bytes", out, V_L2CAP_SMALL,
             sizeof(V_L2CAP_SMALL));

  /* Two fragments, 60 then 44, reassembling a 104 byte L2CAP packet. */
  ds_hci_reasm_reset(&r);
  ds_hci_parse_acl(V_ACL_FRAG1, sizeof(V_ACL_FRAG1), &a);
  checkEq("first fragment needs more",
          ds_hci_reasm_push(&r, &a, &out, &outLen), DS_HCI_REASM_NEED_MORE);
  ds_hci_parse_acl(V_ACL_FRAG2, sizeof(V_ACL_FRAG2), &a);
  checkEq("second fragment completes",
          ds_hci_reasm_push(&r, &a, &out, &outLen), DS_HCI_REASM_DONE);
  checkEq("reassembled length", outLen, sizeof(V_L2CAP_BIG));
  checkBytes("reassembled bytes", out, V_L2CAP_BIG, sizeof(V_L2CAP_BIG));

  /* A continuation with nothing open is a dropped fragment, not data. */
  ds_hci_reasm_reset(&r);
  ds_hci_parse_acl(V_ACL_FRAG2, sizeof(V_ACL_FRAG2), &a);
  checkEq("orphan continuation rejected",
          ds_hci_reasm_push(&r, &a, &out, &outLen), DS_HCI_REASM_ERROR);

  /* A continuation on a different handle must not be spliced in. */
  ds_hci_reasm_reset(&r);
  ds_hci_parse_acl(V_ACL_FRAG1, sizeof(V_ACL_FRAG1), &a);
  ds_hci_reasm_push(&r, &a, &out, &outLen);
  ds_hci_parse_acl(V_ACL_FRAG2, sizeof(V_ACL_FRAG2), &a);
  a.handle = 0x000D;
  checkEq("wrong handle rejected",
          ds_hci_reasm_push(&r, &a, &out, &outLen), DS_HCI_REASM_ERROR);

  /* An L2CAP header claiming more than the reassembly buffer holds. */
  uint8_t huge[8];
  huge[0] = 0xFF; huge[1] = 0xFF;   /* length 65535 */
  huge[2] = 0x41; huge[3] = 0x00;
  ds_hci_reasm_reset(&r);
  a.handle = 0x000C;
  a.pb = DS_HCI_PB_FIRST_FLUSH;
  a.bc = 0;
  a.data = huge;
  a.len = sizeof(huge);
  checkEq("oversized packet rejected",
          ds_hci_reasm_push(&r, &a, &out, &outLen), DS_HCI_REASM_ERROR);

  /* A first fragment too short to hold an L2CAP length field. */
  ds_hci_reasm_reset(&r);
  a.len = 1;
  checkEq("one byte first fragment rejected",
          ds_hci_reasm_push(&r, &a, &out, &outLen), DS_HCI_REASM_ERROR);
}

/* ---------------------------------------------------------------- helpers */

static void testHelpers() {
  Serial.println("helpers");
  char s[18];
  uint8_t addr[DS_HCI_BD_ADDR_LEN];

  ds_hci_addr_to_str(V_ADDR, s);
  check("address prints big endian", strcmp(s, V_ADDR_STR) == 0);
  if (strcmp(s, V_ADDR_STR) != 0) Serial.printf("   got %s\n", s);

  check("address parses back", ds_hci_str_to_addr(V_ADDR_STR, addr));
  checkBytes("round trip", addr, V_ADDR, DS_HCI_BD_ADDR_LEN);

  check("lower case accepted", ds_hci_str_to_addr("11:22:33:44:55:66", addr));
  check("dashes accepted", ds_hci_str_to_addr("11-22-33-44-55-66", addr));
  check("too short rejected", !ds_hci_str_to_addr("11:22:33:44:55", addr));
  check("bad separator rejected",
        !ds_hci_str_to_addr("11:22:33:44:55/66", addr));
  check("non hex rejected", !ds_hci_str_to_addr("11:22:33:44:55:6G", addr));
  check("trailing junk rejected",
        !ds_hci_str_to_addr("11:22:33:44:55:66:77", addr));

  check("event name known",
        strcmp(ds_hci_event_name(DS_HCI_EVT_CONNECTION_COMPLETE),
               "connection complete") == 0);
  check("event name unknown", strcmp(ds_hci_event_name(0xFE), "unknown") == 0);
}

/* ------------------------------------------------------------------- main */

static void runAll() {
  passCount = failCount = 0;
  Serial.println();
  Serial.println("=== DualSense32 HCI self-test ===");
  testOpcodes();
  testCommands();
  testCommandRejects();
  testEvents();
  testInquiry();
  testEventRejects();
  testAcl();
  testReassembly();
  testHelpers();
  Serial.println();
  Serial.printf("%d passed, %d failed -> %s\n", passCount, failCount,
                failCount == 0 ? "PASS" : "FAIL");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  runAll();
}

void loop() {
  delay(4000);
  runAll();
}

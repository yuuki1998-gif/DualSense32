#include <ds_hci_host.h>

#include "turns.h"
#include "vectors.h"

/*
 * HciHost — replays a whole HCI conversation against ds_hci_host.
 *
 * No dongle and no DualSense. The wire is replaced by two functions that just
 * record what was written, so the setup sequence, the pairing handshake and
 * the ACL path can be driven turn by turn and compared byte for byte.
 *
 * The events come from the Bluetooth Core Specification field lists and the
 * expected replies were decided from the specification, not read out of
 * ds_hci_host.c — see gen_host_vectors.py.
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
  Serial.printf("  FAIL  %s  got %lu want %lu\n", name, (unsigned long)got,
                (unsigned long)want);
}

static void checkBytes(const char* name, const uint8_t* got, const uint8_t* want,
                       size_t n) {
  if (memcmp(got, want, n) == 0) { passCount++; return; }
  failCount++;
  Serial.printf("  FAIL  %s\n   got  ", name);
  for (size_t i = 0; i < n && i < 32; i++) Serial.printf("%02X ", got[i]);
  Serial.print("\n   want ");
  for (size_t i = 0; i < n && i < 32; i++) Serial.printf("%02X ", want[i]);
  Serial.println();
}

/* ------------------------------------------------------------- fake wire */

#define MAX_FRAGS 8

static uint8_t cmdBuf[300];
static uint16_t cmdLen;
static int cmdCount;

static uint8_t aclBuf[MAX_FRAGS][320];
static uint16_t aclLen[MAX_FRAGS];
static int aclCount;

/* Observations made through the callbacks. */
static ds_hci_host_state_t lastState;
static int stateChanges;
static bool gotLinkKey;
static uint8_t seenKey[DS_HCI_LINK_KEY_LEN];
static bool haveStoredKey;
static int devicesFound;
static uint32_t foundCod;
static int8_t foundRssi;
static uint8_t foundAddr[DS_HCI_BD_ADDR_LEN];
static int l2capCount;
static uint16_t l2capCid;
static uint16_t l2capLen;
static uint8_t l2capData[320];

static int fakeSendCmd(void* ctx, const uint8_t* pkt, uint16_t len) {
  (void)ctx;
  if (cmdCount == 0 && len <= sizeof(cmdBuf)) {
    memcpy(cmdBuf, pkt, len);
    cmdLen = len;
  }
  cmdCount++;
  return 0;
}

static int fakeSendAcl(void* ctx, const uint8_t* pkt, uint16_t len) {
  (void)ctx;
  if (aclCount < MAX_FRAGS && len <= sizeof(aclBuf[0])) {
    memcpy(aclBuf[aclCount], pkt, len);
    aclLen[aclCount] = len;
  }
  aclCount++;
  return 0;
}

static void cbState(void* ctx, ds_hci_host_state_t st) {
  (void)ctx;
  lastState = st;
  stateChanges++;
}

static void cbLinkKey(void* ctx, const uint8_t addr[6], const uint8_t key[16]) {
  (void)ctx; (void)addr;
  memcpy(seenKey, key, DS_HCI_LINK_KEY_LEN);
  gotLinkKey = true;
}

static bool allowPeer;

static bool cbAcceptPeer(void* ctx, const uint8_t addr[6]) {
  (void)ctx; (void)addr;
  return allowPeer;
}

static bool cbGetLinkKey(void* ctx, const uint8_t addr[6], uint8_t key[16]) {
  (void)ctx; (void)addr;
  if (!haveStoredKey) return false;
  memcpy(key, V_KEY, DS_HCI_LINK_KEY_LEN);
  return true;
}

static void cbDeviceFound(void* ctx, const uint8_t addr[6], uint32_t cod,
                          int8_t rssi, bool hasRssi) {
  (void)ctx; (void)hasRssi;
  memcpy(foundAddr, addr, DS_HCI_BD_ADDR_LEN);
  foundCod = cod;
  foundRssi = rssi;
  devicesFound++;
}

static void cbL2cap(void* ctx, uint16_t cid, const uint8_t* data, uint16_t len) {
  (void)ctx;
  l2capCid = cid;
  l2capLen = len;
  if (len <= sizeof(l2capData)) memcpy(l2capData, data, len);
  l2capCount++;
}

static ds_hci_host_t host;

static void freshHost() {
  ds_hci_host_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.send_cmd = fakeSendCmd;
  cfg.send_acl = fakeSendAcl;
  cfg.on_state = cbState;
  cfg.on_link_key = cbLinkKey;
  cfg.get_link_key = cbGetLinkKey;
  cfg.on_device_found = cbDeviceFound;
  cfg.on_l2cap = cbL2cap;
  cfg.accept_peer = cbAcceptPeer;
  allowPeer = true;
  check("init accepts a zeroed config", ds_hci_host_init(&host, &cfg));
  cmdCount = aclCount = stateChanges = 0;
  devicesFound = l2capCount = 0;
  gotLinkKey = false;
}

/* ------------------------------------------------------------ turn script */

#define TURN(label, feed, want) \
  { label, feed, (uint16_t)sizeof(feed), want, (uint16_t)sizeof(want) }
#define TURN_SILENT(label, feed) \
  { label, feed, (uint16_t)sizeof(feed), NULL, 0 }

static void expectCmd(const char* label, const uint8_t* want, uint16_t wantLen) {
  if (cmdCount != 1) {
    failCount++;
    Serial.printf("  FAIL  %s  emitted %d commands, wanted 1\n", label,
                  cmdCount);
    return;
  }
  if (cmdLen != wantLen) {
    failCount++;
    Serial.printf("  FAIL  %s  length %u want %u\n", label, cmdLen, wantLen);
    return;
  }
  checkBytes(label, cmdBuf, want, wantLen);
}

static void runScript(const turn_t* t, size_t n) {
  for (size_t i = 0; i < n; i++) {
    cmdCount = 0;
    if (t[i].feed != NULL) {
      ds_hci_host_handle_event(&host, t[i].feed, t[i].feedLen);
    }
    if (t[i].want != NULL) {
      expectCmd(t[i].label, t[i].want, t[i].wantLen);
    } else {
      check(t[i].label, cmdCount == 0);
    }
  }
}

/* --------------------------------------------------------- setup sequence */

static void bringUp() {
  static const turn_t script[] = {
    TURN("reset ok -> read version",       V_EVT_1_RESET_OK,   V_CMD_2_VERSION),
    TURN("version -> local commands",      V_EVT_2_VERSION_OK, V_CMD_2B_LOCAL_CMDS),
    TURN("local commands -> local features", V_EVT_2B_OK,      V_CMD_2C_LOCAL_FEAT),
    TURN("local features -> buffer size",  V_EVT_2C_OK,        V_CMD_3_BUFSIZE),
    TURN("buffer size -> read bd addr",    V_EVT_3_BUFSIZE_OK, V_CMD_4_BDADDR),
    TURN("bd addr -> set event mask",      V_EVT_4_BDADDR_OK,  V_CMD_5_EVENT_MASK),
    TURN("event mask -> page 2",           V_EVT_5_OK,         V_CMD_5C_EVENT_MASK_P2),
    TURN("page 2 -> link policy",          V_EVT_5C_OK,        V_CMD_5B_POLICY),
    TURN("link policy -> simple pairing",  V_EVT_5B_OK,        V_CMD_8_SSP),
    TURN("simple pairing -> secure connections", V_EVT_8_OK,   V_CMD_8B_SECURE_CONN),
    TURN("secure connections -> inquiry mode", V_EVT_8B_OK,    V_CMD_8C_INQ_MODE),
    TURN("inquiry mode -> class of device", V_EVT_8C_OK,       V_CMD_6_COD),
    TURN("class of device -> local name",  V_EVT_6_OK,         V_CMD_7_NAME),
    TURN("local name -> page timeout",     V_EVT_7_OK,         V_CMD_8D_PAGE_TIMEOUT),
    TURN("page timeout -> page scan activity", V_EVT_8D_OK,    V_CMD_8E_PSCAN_ACT),
    TURN("page scan activity -> page scan type", V_EVT_8E_OK,  V_CMD_8F_PSCAN_TYPE),
    TURN("page scan type -> scan enable",  V_EVT_8F_OK,        V_CMD_9_SCAN),
    TURN_SILENT("scan enable ends setup",  V_EVT_9_OK),
  };
  cmdCount = 0;
  check("start", ds_hci_host_start(&host));
  expectCmd("start sends reset", V_CMD_1_RESET, sizeof(V_CMD_1_RESET));
  runScript(script, sizeof(script) / sizeof(script[0]));
}

static void testSetup() {
  Serial.println("setup sequence");
  freshHost();
  checkEq("starts idle", ds_hci_host_state(&host), DS_HOST_IDLE);
  bringUp();
  checkEq("ends ready", ds_hci_host_state(&host), DS_HOST_READY);

  /* What the controller told us on the way through. */
  checkEq("hci version captured", host.hci_version, 14);
  checkEq("manufacturer captured", host.manufacturer, 2279);
  checkEq("acl mtu captured", host.acl_mtu, 1021);
  checkEq("acl credits captured", host.acl_credits, 4);
  checkBytes("local address captured", host.local_addr, V_LOCAL,
             DS_HCI_BD_ADDR_LEN);
}

static void testSetupFailure() {
  Serial.println("setup failure");
  freshHost();
  ds_hci_host_start(&host);
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_RESET_FAIL, sizeof(V_EVT_RESET_FAIL));
  checkEq("rejected step stops the sequence", ds_hci_host_state(&host),
          DS_HOST_FAILED);
  check("no further command sent", cmdCount == 0);

  /* An event for a command we are not waiting on must be ignored, not
   * mistaken for the current step. */
  freshHost();
  ds_hci_host_start(&host);
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_5_OK, sizeof(V_EVT_5_OK));
  check("unrelated command complete ignored", cmdCount == 0);
  checkEq("still initialising", ds_hci_host_state(&host),
          DS_HOST_INITIALISING);
}

/* ------------------------------------------------------- connect and pair */

static void testPairing() {
  Serial.println("connect and pair");
  static const turn_t script[] = {
    TURN_SILENT("page accepted, still waiting", V_EVT_CREATE_CONN_PENDING),
    /* The link is up and its features are read before anything is asked of
     * it, which is what the remote extended features page is for: it says
     * whether secure connections are on the table. */
    TURN("acl up -> read remote features", V_EVT_CONN_COMPLETE,
         V_CMD_REMOTE_FEAT),
    TURN("features -> extended features", V_EVT_REMOTE_FEAT, V_CMD_REMOTE_EXT),
    TURN("extended features -> request authentication", V_EVT_REMOTE_EXT,
         V_CMD_AUTH_REQ),
    TURN("no stored key -> negative reply", V_EVT_LINK_KEY_REQ,
         V_CMD_LINK_KEY_NEG),
    TURN("io capability request -> reply", V_EVT_IOCAP_REQ, V_CMD_IOCAP_REPLY),
    TURN_SILENT("io capability response needs no reply", V_EVT_IOCAP_RESPONSE),
    TURN("user confirmation -> just works accept", V_EVT_USER_CONF,
         V_CMD_USER_CONF_ACCEPT),
    TURN_SILENT("simple pairing complete", V_EVT_SSP_COMPLETE),
    TURN_SILENT("link key notification", V_EVT_LINK_KEY_NOTIF),
    TURN("authenticated -> turn on encryption", V_EVT_AUTH_COMPLETE,
         V_CMD_SET_ENC),
    TURN("encryption on -> read key size", V_EVT_ENC_ON, V_CMD_ENC_KEY_SIZE),
  };

  freshHost();
  bringUp();
  haveStoredKey = false;

  cmdCount = 0;
  check("connect", ds_hci_host_connect(&host, V_PEER));
  expectCmd("connect pages the peer", V_CMD_CREATE_CONN,
            sizeof(V_CMD_CREATE_CONN));
  checkEq("connecting", ds_hci_host_state(&host), DS_HOST_CONNECTING);

  runScript(script, sizeof(script) / sizeof(script[0]));

  checkEq("connected", ds_hci_host_state(&host), DS_HOST_CONNECTED);
  check("is_connected agrees", ds_hci_host_is_connected(&host));
  checkEq("handle stored", host.handle, V_HANDLE);
  checkBytes("peer stored", host.peer_addr, V_PEER, DS_HCI_BD_ADDR_LEN);
  check("link key handed to the app", gotLinkKey);
  checkBytes("link key value", seenKey, V_KEY, DS_HCI_LINK_KEY_LEN);

  /* And back down again. */
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_DISCONN, sizeof(V_EVT_DISCONN));
  checkEq("disconnect returns to ready", ds_hci_host_state(&host),
          DS_HOST_READY);
}

static void testStoredKey() {
  Serial.println("reconnect with a stored key");
  freshHost();
  bringUp();
  haveStoredKey = true;

  ds_hci_host_connect(&host, V_PEER);
  ds_hci_host_handle_event(&host, V_EVT_CONN_COMPLETE,
                            sizeof(V_EVT_CONN_COMPLETE));
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_LINK_KEY_REQ,
                            sizeof(V_EVT_LINK_KEY_REQ));
  expectCmd("stored key is replied with", V_CMD_LINK_KEY_REPLY,
            sizeof(V_CMD_LINK_KEY_REPLY));

  /* Straight to authenticated, no pairing dance. */
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_AUTH_COMPLETE,
                            sizeof(V_EVT_AUTH_COMPLETE));
  expectCmd("authenticated -> encryption", V_CMD_SET_ENC, sizeof(V_CMD_SET_ENC));
  ds_hci_host_handle_event(&host, V_EVT_ENC_ON, sizeof(V_EVT_ENC_ON));
  checkEq("connected without pairing", ds_hci_host_state(&host),
          DS_HOST_CONNECTED);
  check("no new link key stored", !gotLinkKey);
}

static void testFailures() {
  Serial.println("connection failures");

  freshHost();
  bringUp();
  ds_hci_host_connect(&host, V_PEER);
  ds_hci_host_handle_event(&host, V_EVT_CREATE_CONN_REFUSED,
                            sizeof(V_EVT_CREATE_CONN_REFUSED));
  checkEq("refused page fails", ds_hci_host_state(&host), DS_HOST_FAILED);
  check("can retry from failed", ds_hci_host_connect(&host, V_PEER));

  freshHost();
  bringUp();
  ds_hci_host_connect(&host, V_PEER);
  ds_hci_host_handle_event(&host, V_EVT_CONN_FAILED, sizeof(V_EVT_CONN_FAILED));
  checkEq("page timeout fails", ds_hci_host_state(&host), DS_HOST_FAILED);

  freshHost();
  bringUp();
  ds_hci_host_connect(&host, V_PEER);
  ds_hci_host_handle_event(&host, V_EVT_CONN_COMPLETE,
                            sizeof(V_EVT_CONN_COMPLETE));
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_AUTH_FAILED, sizeof(V_EVT_AUTH_FAILED));
  checkEq("failed authentication stops", ds_hci_host_state(&host),
          DS_HOST_FAILED);
  /*
   * Not "no encryption attempted" any more: the ACL from V_EVT_CONN_COMPLETE
   * is still live at the controller when authentication fails, and fail()
   * has to say so -- otherwise a retry that pages the same address gets ACL
   * Connection Already Exists (0x0B) against a link this host never
   * actually gave up.
   */
  expectCmd("and disconnects the still-live ACL", V_CMD_DISCONNECT,
            sizeof(V_CMD_DISCONNECT));
  check("credits given back on failure too", host.acl_credits > 0);
}

static void testInbound() {
  Serial.println("inbound connection");
  freshHost();
  bringUp();

  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_CONN_REQUEST,
                            sizeof(V_EVT_CONN_REQUEST));
  expectCmd("incoming page is accepted", V_CMD_ACCEPT_CONN,
            sizeof(V_CMD_ACCEPT_CONN));
  checkEq("connecting", ds_hci_host_state(&host), DS_HOST_CONNECTING);

  /*
   * An inbound link is not driven from this end. A DualSense that pages does
   * service discovery first and asks for the link key afterwards, and cutting
   * in with Authentication Requested abandons the discovery half done -- the
   * pad then waits thirty seconds, gives up and pages again.
   *
   * This test used to assert the opposite, and the link really did sit there
   * when it was written that way. The reason was elsewhere: the SDP channel
   * was refused and every reply built before encryption was dropped a layer
   * above the USB, so the pad never got far enough to ask for anything.
   */
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_CONN_COMPLETE,
                            sizeof(V_EVT_CONN_COMPLETE));
  expectCmd("inbound reads the features too", V_CMD_REMOTE_FEAT,
            sizeof(V_CMD_REMOTE_FEAT));

  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_REMOTE_FEAT, sizeof(V_EVT_REMOTE_FEAT));
  expectCmd("and the extended ones", V_CMD_REMOTE_EXT, sizeof(V_CMD_REMOTE_EXT));

  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_REMOTE_EXT, sizeof(V_EVT_REMOTE_EXT));
  check("then leaves authentication to the pad", cmdCount == 0);

  haveStoredKey = true;
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_LINK_KEY_REQ,
                            sizeof(V_EVT_LINK_KEY_REQ));
  expectCmd("and answers the key request", V_CMD_LINK_KEY_REPLY,
            sizeof(V_CMD_LINK_KEY_REPLY));

  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_AUTH_COMPLETE,
                            sizeof(V_EVT_AUTH_COMPLETE));
  expectCmd("and turns encryption on", V_CMD_SET_ENC, sizeof(V_CMD_SET_ENC));

  ds_hci_host_handle_event(&host, V_EVT_ENC_ON, sizeof(V_EVT_ENC_ON));
  checkEq("inbound reaches connected", ds_hci_host_state(&host),
          DS_HOST_CONNECTED);

  /* The reason a failure gives is the whole diagnosis: keys disagreeing is
   * not the same problem as the peer having no key at all. */
  freshHost();
  bringUp();
  ds_hci_host_handle_event(&host, V_EVT_CONN_REQUEST,
                            sizeof(V_EVT_CONN_REQUEST));
  ds_hci_host_handle_event(&host, V_EVT_CONN_COMPLETE,
                            sizeof(V_EVT_CONN_COMPLETE));
  ds_hci_host_handle_event(&host, V_EVT_AUTH_FAILED,
                            sizeof(V_EVT_AUTH_FAILED));
  checkEq("failed authentication is recorded", host.last_auth_status, 0x05);
  check("and it has a name",
        strcmp(ds_hci_error_name(0x05), "authentication failure") == 0);
  check("so does a missing key",
        strcmp(ds_hci_error_name(0x06), "pin or key missing") == 0);

  /* Once a pad is bonded, only that pad may call in. A stranger that got as
   * far as pairing would take the one bond slot, and the real pad would then
   * fail authentication on every PS press until it was paired again. It is
   * turned away before anything else happens, with the reason that says so. */
  freshHost();
  bringUp();
  allowPeer = false;
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_CONN_REQUEST,
                            sizeof(V_EVT_CONN_REQUEST));
  expectCmd("a caller that is not ours is rejected", V_CMD_REJECT_CONN,
            sizeof(V_CMD_REJECT_CONN));
  checkEq("and nothing starts", ds_hci_host_state(&host), DS_HOST_READY);

  /* Busy paging is no reason to leave a stranger hanging for the controller's
   * accept timeout: say no straight away, and keep the page going. */
  ds_hci_host_connect(&host, V_PEER);
  cmdCount = 0;
  ds_hci_host_handle_event(&host, V_EVT_CONN_REQUEST,
                            sizeof(V_EVT_CONN_REQUEST));
  expectCmd("rejected while paging too", V_CMD_REJECT_CONN,
            sizeof(V_CMD_REJECT_CONN));
  checkEq("and the page carries on", ds_hci_host_state(&host),
          DS_HOST_CONNECTING);
  check("still paging, not inbound", !host.inbound);
}

static void testInquiry() {
  Serial.println("inquiry");
  freshHost();
  bringUp();

  check("inquiry starts", ds_hci_host_inquiry(&host, 10));
  checkEq("inquiring", ds_hci_host_state(&host), DS_HOST_INQUIRING);

  ds_hci_host_handle_event(&host, V_EVT_INQ_GAMEPAD,
                            sizeof(V_EVT_INQ_GAMEPAD));
  checkEq("one device reported", devicesFound, 1);
  checkBytes("device address", foundAddr, V_PEER, DS_HCI_BD_ADDR_LEN);
  checkEq("device class", foundCod, V_COD_GAMEPAD);
  checkEq("device rssi", (uint32_t)(int32_t)foundRssi, (uint32_t)(int32_t)-50);
  check("class recognised as a gamepad", ds_hci_cod_is_gamepad(foundCod));
  check("a headset is not a gamepad", !ds_hci_cod_is_gamepad(0x240404));

  ds_hci_host_handle_event(&host, V_EVT_INQ_COMPLETE,
                            sizeof(V_EVT_INQ_COMPLETE));
  checkEq("inquiry complete returns to ready", ds_hci_host_state(&host),
          DS_HOST_READY);
}

/* ------------------------------------------------------------------- acl */

/* Drives a fresh host to CONNECTED, but with a controller that only accepts
 * 32 byte ACL payloads so that fragmentation actually happens. */
static void bringUpSmallBuffers() {
  /* Command Complete for Read Buffer Size: acl length 32, 4 buffers. */
  static const uint8_t bufsize32[] = {
    0x0E, 0x0B, 0x01, 0x05, 0x10, 0x00, 0x20, 0x00, 0x08, 0x04, 0x00, 0x08,
    0x00,
  };
  /* The whole setup, with the small buffer size standing in for the real
   * one. Listed rather than spelled out so that a sequence that grows again
   * is one line here, not nine. */
  static const struct { const uint8_t* e; uint16_t n; } setup[] = {
    {V_EVT_1_RESET_OK, sizeof(V_EVT_1_RESET_OK)},
    {V_EVT_2_VERSION_OK, sizeof(V_EVT_2_VERSION_OK)},
    {V_EVT_2B_OK, sizeof(V_EVT_2B_OK)},
    {V_EVT_2C_OK, sizeof(V_EVT_2C_OK)},
    {bufsize32, sizeof(bufsize32)},
    {V_EVT_4_BDADDR_OK, sizeof(V_EVT_4_BDADDR_OK)},
    {V_EVT_5_OK, sizeof(V_EVT_5_OK)},
    {V_EVT_5C_OK, sizeof(V_EVT_5C_OK)},
    {V_EVT_5B_OK, sizeof(V_EVT_5B_OK)},
    {V_EVT_8_OK, sizeof(V_EVT_8_OK)},
    {V_EVT_8B_OK, sizeof(V_EVT_8B_OK)},
    {V_EVT_8C_OK, sizeof(V_EVT_8C_OK)},
    {V_EVT_6_OK, sizeof(V_EVT_6_OK)},
    {V_EVT_7_OK, sizeof(V_EVT_7_OK)},
    {V_EVT_8D_OK, sizeof(V_EVT_8D_OK)},
    {V_EVT_8E_OK, sizeof(V_EVT_8E_OK)},
    {V_EVT_8F_OK, sizeof(V_EVT_8F_OK)},
    {V_EVT_9_OK, sizeof(V_EVT_9_OK)},
  };
  freshHost();
  ds_hci_host_start(&host);
  for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
    ds_hci_host_handle_event(&host, setup[i].e, setup[i].n);
  }

  haveStoredKey = true;
  ds_hci_host_connect(&host, V_PEER);
  ds_hci_host_handle_event(&host, V_EVT_CONN_COMPLETE,
                            sizeof(V_EVT_CONN_COMPLETE));
  ds_hci_host_handle_event(&host, V_EVT_REMOTE_FEAT,
                            sizeof(V_EVT_REMOTE_FEAT));
  ds_hci_host_handle_event(&host, V_EVT_REMOTE_EXT,
                            sizeof(V_EVT_REMOTE_EXT));
  ds_hci_host_handle_event(&host, V_EVT_LINK_KEY_REQ,
                            sizeof(V_EVT_LINK_KEY_REQ));
  ds_hci_host_handle_event(&host, V_EVT_AUTH_COMPLETE,
                            sizeof(V_EVT_AUTH_COMPLETE));
  ds_hci_host_handle_event(&host, V_EVT_ENC_ON, sizeof(V_EVT_ENC_ON));
}

static void testAclReceive() {
  Serial.println("acl receive");
  freshHost();
  bringUp();
  haveStoredKey = true;
  ds_hci_host_connect(&host, V_PEER);
  ds_hci_host_handle_event(&host, V_EVT_CONN_COMPLETE,
                            sizeof(V_EVT_CONN_COMPLETE));
  ds_hci_host_handle_event(&host, V_EVT_LINK_KEY_REQ,
                            sizeof(V_EVT_LINK_KEY_REQ));
  ds_hci_host_handle_event(&host, V_EVT_AUTH_COMPLETE,
                            sizeof(V_EVT_AUTH_COMPLETE));
  ds_hci_host_handle_event(&host, V_EVT_ENC_ON, sizeof(V_EVT_ENC_ON));

  ds_hci_host_handle_acl(&host, V_ACL_IN, sizeof(V_ACL_IN));
  checkEq("one l2cap packet handed up", l2capCount, 1);
  checkEq("hid interrupt channel", l2capCid, 0x0041);
  checkEq("payload length", l2capLen, sizeof(V_HID_REPORT));
  checkBytes("payload", l2capData, V_HID_REPORT, sizeof(V_HID_REPORT));

  /* Traffic on a handle we do not own must not be handed up. */
  uint8_t other[sizeof(V_ACL_IN)];
  memcpy(other, V_ACL_IN, sizeof(V_ACL_IN));
  other[0] = 0x0D;                     /* handle 0x000D */
  l2capCount = 0;
  ds_hci_host_handle_acl(&host, other, sizeof(other));
  check("foreign handle ignored", l2capCount == 0);
}

static void testAclSend() {
  Serial.println("acl send and fragmentation");
  bringUpSmallBuffers();
  checkEq("small buffers taken", host.acl_mtu, 32);
  checkEq("four credits", host.acl_credits, 4);
  checkEq("connected", ds_hci_host_state(&host), DS_HOST_CONNECTED);

  /* A 79 byte DualSense output report over a 32 byte ACL buffer: the 4 byte
   * L2CAP header leaves 28 in the first fragment, then 32 and 19. */
  uint8_t report[79];
  for (size_t i = 0; i < sizeof(report); i++) report[i] = (uint8_t)(i * 5 + 2);

  aclCount = 0;
  check("send accepted", ds_hci_host_send_l2cap(&host, 0x0041, report,
                                                 sizeof(report)));
  checkEq("three fragments", aclCount, 3);
  checkEq("first fragment size", aclLen[0], 4 + 32);
  checkEq("second fragment size", aclLen[1], 4 + 32);
  checkEq("third fragment size", aclLen[2], 4 + 19);
  checkEq("credits spent", host.acl_credits, 1);

  /* Feed the fragments back through the reassembler: what comes out must be
   * exactly the L2CAP packet we asked for. */
  ds_hci_reasm_t r;
  ds_hci_acl_t a;
  const uint8_t* whole = NULL;
  uint16_t wholeLen = 0;
  ds_hci_reasm_reset(&r);
  ds_hci_reasm_result_t res = DS_HCI_REASM_ERROR;
  for (int i = 0; i < 3; i++) {
    check("fragment parses", ds_hci_parse_acl(aclBuf[i], aclLen[i], &a));
    checkEq("fragment handle", a.handle, V_HANDLE);
    checkEq("boundary flag", a.pb,
            i == 0 ? DS_HCI_PB_FIRST_FLUSH : DS_HCI_PB_CONTINUING);
    res = ds_hci_reasm_push(&r, &a, &whole, &wholeLen);
  }
  checkEq("round trip completes", res, DS_HCI_REASM_DONE);
  checkEq("round trip length", wholeLen, 4 + sizeof(report));
  checkEq("round trip l2cap length field",
          (uint16_t)(whole[0] | ((uint16_t)whole[1] << 8)), sizeof(report));
  checkEq("round trip channel id",
          (uint16_t)(whole[2] | ((uint16_t)whole[3] << 8)), 0x0041);
  checkBytes("round trip payload", whole + 4, report, sizeof(report));

  /* One credit left, three needed. */
  aclCount = 0;
  check("send refused without credits",
        !ds_hci_host_send_l2cap(&host, 0x0041, report, sizeof(report)));
  check("nothing was put on the wire", aclCount == 0);

  /* The controller drains two buffers. */
  ds_hci_host_handle_event(&host, V_EVT_COMPLETED_2,
                            sizeof(V_EVT_COMPLETED_2));
  checkEq("credits returned", host.acl_credits, 3);
  check("send accepted again",
        ds_hci_host_send_l2cap(&host, 0x0041, report, sizeof(report)));

  /* Credits must never climb past what the controller actually has. */
  for (int i = 0; i < 5; i++) {
    ds_hci_host_handle_event(&host, V_EVT_COMPLETED_2,
                              sizeof(V_EVT_COMPLETED_2));
  }
  checkEq("credits clamped to the total", host.acl_credits, 4);

  /* A short report fits in one fragment. */
  aclCount = 0;
  uint8_t small[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  check("small send accepted",
        ds_hci_host_send_l2cap(&host, 0x0040, small, sizeof(small)));
  checkEq("one fragment", aclCount, 1);
  checkEq("one fragment size", aclLen[0], 4 + 4 + 8);

  /* A pad that goes silent leaves packets in the controller that will never
   * be acknowledged, and the spec says a Disconnection Complete frees them.
   * Keeping them spent means the next PS-button connection starts with no
   * credits, cannot answer the pad's SDP, and is dropped thirty seconds on. */
  ds_hci_host_send_l2cap(&host, 0x0041, small, sizeof(small));
  check("credits spent before the drop", host.acl_credits < 4);
  ds_hci_host_handle_event(&host, V_EVT_DISCONN, sizeof(V_EVT_DISCONN));
  checkEq("disconnect gives every credit back", host.acl_credits, 4);
}

static void testSendRejects() {
  Serial.println("send rejects");
  uint8_t small[4] = {1, 2, 3, 4};

  freshHost();
  check("send before setup refused",
        !ds_hci_host_send_l2cap(&host, 0x0041, small, sizeof(small)));
  check("inquiry before setup refused", !ds_hci_host_inquiry(&host, 10));
  check("connect before setup refused", !ds_hci_host_connect(&host, V_PEER));

  bringUp();
  check("send while merely ready refused",
        !ds_hci_host_send_l2cap(&host, 0x0041, small, sizeof(small)));
  check("disconnect without a link refused", !ds_hci_host_disconnect(&host));
  check("null payload with a length refused",
        !ds_hci_host_send_l2cap(&host, 0x0041, NULL, 4));

  /* A second connect while one is already in flight must not clobber it. */
  ds_hci_host_connect(&host, V_PEER);
  check("connect while connecting refused",
        !ds_hci_host_connect(&host, V_PEER));
}

/* ------------------------------------------------------------------- main */

static void runAll() {
  passCount = failCount = 0;
  Serial.println();
  Serial.println("=== DualSense32 HCI host self-test ===");
  testSetup();
  testSetupFailure();
  testPairing();
  testStoredKey();
  testFailures();
  testInbound();
  testInquiry();
  testAclReceive();
  testAclSend();
  testSendRejects();
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

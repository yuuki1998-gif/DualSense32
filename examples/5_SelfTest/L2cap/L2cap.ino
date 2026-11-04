#include <ds_l2cap.h>
#include <ds_sdp.h>

#include "vectors.h"

/*
 * L2cap — replays the L2CAP channel handshake.
 *
 * No controller and no radio. The wire is a recording function, so the whole
 * exchange that brings up the two HID channels can be driven packet by packet
 * and checked against what the specification says must go out.
 *
 * Signalling identifiers are deliberately not baked into the expected bytes:
 * the test checks the command code and the body, and separately checks that
 * every response echoes the identifier of the request it answers. Pinning the
 * identifier would only test the counter. See gen_l2cap_vectors.py.
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
  for (size_t i = 0; i < n && i < 32; i++) Serial.printf("%02X ", got[i]);
  Serial.print("\n   want ");
  for (size_t i = 0; i < n && i < 32; i++) Serial.printf("%02X ", want[i]);
  Serial.println();
}

/* ------------------------------------------------------------- fake wire */

#define MAX_SENDS 4

static uint16_t sentCid[MAX_SENDS];
static uint8_t sentBuf[MAX_SENDS][128];
static uint16_t sentLen[MAX_SENDS];
static int sentCount;
static bool sendFails;

static int fakeSend(void* ctx, uint16_t cid, const uint8_t* data, uint16_t len) {
  (void)ctx;
  if (sendFails) return -1;
  if (sentCount < MAX_SENDS && len <= sizeof(sentBuf[0])) {
    sentCid[sentCount] = cid;
    memcpy(sentBuf[sentCount], data, len);
    sentLen[sentCount] = len;
  }
  sentCount++;
  return 0;
}

static int opensSeen, closesSeen, dataSeen;
static uint16_t lastOpenPsm, lastClosePsm, lastDataPsm, lastDataLen;
static uint8_t lastData[128];

static void cbOpen(void* ctx, uint16_t psm) {
  (void)ctx; lastOpenPsm = psm; opensSeen++;
}
static void cbClose(void* ctx, uint16_t psm) {
  (void)ctx; lastClosePsm = psm; closesSeen++;
}
static void cbData(void* ctx, uint16_t psm, const uint8_t* d, uint16_t n) {
  (void)ctx;
  lastDataPsm = psm;
  lastDataLen = n;
  if (n <= sizeof(lastData)) memcpy(lastData, d, n);
  dataSeen++;
}

static ds_l2cap_t l2;

static void freshL2cap() {
  ds_l2cap_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.send = fakeSend;
  cfg.on_open = cbOpen;
  cfg.on_close = cbClose;
  cfg.on_data = cbData;
  check("init accepts a zeroed config", ds_l2cap_init(&l2, &cfg));
  sentCount = opensSeen = closesSeen = dataSeen = 0;
  sendFails = false;
}

/* Checks the nth thing put on the wire: it must be a signalling packet with
 * this command code and this body. Returns the identifier it carried. */
static uint8_t expectSig(const char* label, int n, uint8_t code,
                         const uint8_t* body, uint16_t bodyLen) {
  if (n >= sentCount) {
    failCount++;
    Serial.printf("  FAIL  %s  nothing sent (%d packets)\n", label, sentCount);
    return 0;
  }
  if (sentCid[n] != DS_L2CAP_CID_SIGNALLING) {
    failCount++;
    Serial.printf("  FAIL  %s  went to cid 0x%04X\n", label, sentCid[n]);
    return 0;
  }
  if (sentLen[n] != 4 + bodyLen) {
    failCount++;
    Serial.printf("  FAIL  %s  length %u want %u\n", label, sentLen[n],
                  4 + bodyLen);
    return 0;
  }
  if (sentBuf[n][0] != code) {
    failCount++;
    Serial.printf("  FAIL  %s  code 0x%02X want 0x%02X\n", label, sentBuf[n][0],
                  code);
    return 0;
  }
  /* The length inside the packet must agree with the packet itself. */
  uint16_t inner = (uint16_t)(sentBuf[n][2] | ((uint16_t)sentBuf[n][3] << 8));
  if (inner != bodyLen) {
    failCount++;
    Serial.printf("  FAIL  %s  inner length %u want %u\n", label, inner,
                  bodyLen);
    return 0;
  }
  checkBytes(label, sentBuf[n] + 4, body, bodyLen);
  return sentBuf[n][1];
}

#define FEED(v) ds_l2cap_handle(&l2, DS_L2CAP_CID_SIGNALLING, v, sizeof(v))
#define EXPECT(label, n, code, body) \
  expectSig(label, n, code, body, (uint16_t)sizeof(body))

/* ------------------------------------------------------- bringing up HID */

/* Drives both channels to open. Returns with sentCount reset to 0. */
static void openHid() {
  sentCount = 0;
  ds_l2cap_connect_hid(&l2);
  EXPECT("control connection request", 0, DS_L2CAP_CONN_REQ,
         V_OUT_CONN_REQ_CTRL);

  sentCount = 0;
  FEED(V_IN_CONN_RSP_CTRL);
  EXPECT("control config request", 0, DS_L2CAP_CONFIG_REQ,
         V_OUT_CONFIG_REQ_CTRL);

  sentCount = 0;
  FEED(V_IN_CONFIG_REQ_CTRL);
  uint8_t id = EXPECT("control config response", 0, DS_L2CAP_CONFIG_RSP,
                      V_OUT_CONFIG_RSP_CTRL);
  checkEq("response echoes their identifier", id, V_ID_CFG_REQ_CTRL);
  check("not open on one half of the config", !ds_l2cap_is_open(&l2, 0x0011));

  sentCount = 0;
  FEED(V_IN_CONFIG_RSP_CTRL);
  check("control open", ds_l2cap_is_open(&l2, DS_L2CAP_PSM_HID_CONTROL));
  EXPECT("interrupt follows control", 0, DS_L2CAP_CONN_REQ,
         V_OUT_CONN_REQ_INTR);

  sentCount = 0;
  FEED(V_IN_CONN_RSP_INTR);
  EXPECT("interrupt config request", 0, DS_L2CAP_CONFIG_REQ,
         V_OUT_CONFIG_REQ_INTR);

  sentCount = 0;
  FEED(V_IN_CONFIG_REQ_INTR);
  id = EXPECT("interrupt config response", 0, DS_L2CAP_CONFIG_RSP,
              V_OUT_CONFIG_RSP_INTR);
  checkEq("interrupt response echoes identifier", id, V_ID_CFG_REQ_INTR);

  sentCount = 0;
  FEED(V_IN_CONFIG_RSP_INTR);
}

static void testHandshake() {
  Serial.println("hid channel handshake");
  freshL2cap();
  check("nothing open to begin with", !ds_l2cap_hid_ready(&l2));
  openHid();

  check("both channels open", ds_l2cap_hid_ready(&l2));
  checkEq("two open callbacks", opensSeen, 2);
  checkEq("interrupt opened last", lastOpenPsm, DS_L2CAP_PSM_HID_INTERRUPT);

  const ds_l2cap_chan_t* c =
      ds_l2cap_channel(&l2, DS_L2CAP_PSM_HID_INTERRUPT);
  check("interrupt channel exists", c != NULL);
  if (c != NULL) {
    checkEq("local cid", c->local_cid, V_LCID_INTR);
    checkEq("remote cid", c->remote_cid, V_RCID_INTR);
    checkEq("their mtu remembered", c->remote_mtu, 1024);
    checkEq("state name", strcmp(ds_l2cap_state_name(c->state), "open"), 0);
  }
}

/* ------------------------------------------------------------ traffic --- */

static void testTraffic() {
  Serial.println("traffic");
  freshL2cap();
  openHid();

  /* Inbound: a report arriving on our interrupt cid is handed up by PSM. */
  dataSeen = 0;
  ds_l2cap_handle(&l2, V_LCID_INTR, V_HID_REPORT, sizeof(V_HID_REPORT));
  checkEq("one report handed up", dataSeen, 1);
  checkEq("came from the interrupt psm", lastDataPsm,
          DS_L2CAP_PSM_HID_INTERRUPT);
  checkEq("full length", lastDataLen, sizeof(V_HID_REPORT));
  checkBytes("payload intact", lastData, V_HID_REPORT, sizeof(V_HID_REPORT));

  /* An unknown channel id is not our traffic. */
  dataSeen = 0;
  ds_l2cap_handle(&l2, 0x0099, V_HID_REPORT, sizeof(V_HID_REPORT));
  check("unknown cid dropped", dataSeen == 0);

  /* Outbound: goes to the remote's cid, not ours. */
  sentCount = 0;
  uint8_t out[79];
  for (size_t i = 0; i < sizeof(out); i++) out[i] = (uint8_t)(i + 1);
  check("send on interrupt", ds_l2cap_send(&l2, DS_L2CAP_PSM_HID_INTERRUPT,
                                            out, sizeof(out)));
  checkEq("one packet", sentCount, 1);
  checkEq("addressed to their cid", sentCid[0], V_RCID_INTR);
  checkEq("length", sentLen[0], sizeof(out));
  checkBytes("bytes", sentBuf[0], out, sizeof(out));

  /* Their MTU was 1024; more than that must be refused rather than dropped
   * somewhere further down where nobody would notice. */
  static uint8_t big[1100];
  check("over their mtu refused",
        !ds_l2cap_send(&l2, DS_L2CAP_PSM_HID_INTERRUPT, big, sizeof(big)));
  check("send on a psm we never opened",
        !ds_l2cap_send(&l2, DS_L2CAP_PSM_SDP, out, 4));
}

/* --------------------------------------------------------- inbound open - */

static void testInboundOpen() {
  Serial.println("channel opened by the peer");
  freshL2cap();

  sentCount = 0;
  FEED(V_IN_CONN_REQ_CTRL);
  uint8_t id = EXPECT("accepted", 0, DS_L2CAP_CONN_RSP, V_OUT_CONN_RSP_ACCEPT);
  checkEq("accept echoes their identifier", id, V_ID_CONN_REQ_IN);
  EXPECT("and we configure straight away", 1, DS_L2CAP_CONFIG_REQ,
         V_OUT_CONFIG_REQ_CTRL);
  checkEq("two packets went out", sentCount, 2);

  /* A PSM we do not serve must be refused, not accepted and then ignored. */
  freshL2cap();
  sentCount = 0;
  FEED(V_IN_CONN_REQ_BAD_PSM);
  id = EXPECT("unsupported psm refused", 0, DS_L2CAP_CONN_RSP,
              V_OUT_CONN_RSP_NO_PSM);
  checkEq("refusal echoes identifier", id, V_ID_BAD_PSM);
  checkEq("only one packet", sentCount, 1);
}

/* Both ends deciding to open the same channel at the same moment is not a
 * theoretical case: it showed up on real hardware as a link that connected and
 * then vanished, because refusing the peer's request made it give up. An
 * attempt of ours that has not opened yet is worth less than a request already
 * in hand, so it gives way. */
static void testOpenCollision() {
  Serial.println("both ends opening at once");
  freshL2cap();

  check("our request goes out", ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL));
  sentCount = 0;
  closesSeen = 0;
  FEED(V_IN_CONN_REQ_CTRL);

  uint8_t id = EXPECT("theirs is accepted", 0, DS_L2CAP_CONN_RSP,
                      V_OUT_CONN_RSP_ACCEPT_2);
  checkEq("accept echoes their identifier", id, V_ID_CONN_REQ_IN);
  EXPECT("and we configure it", 1, DS_L2CAP_CONFIG_REQ, V_OUT_CONFIG_REQ_CTRL);
  checkEq("our half-built attempt was dropped", closesSeen, 1);

  const ds_l2cap_chan_t* c = ds_l2cap_channel(&l2, DS_L2CAP_PSM_HID_CONTROL);
  check("one channel survives", c != NULL);
  if (c != NULL) {
    checkEq("it is theirs", c->remote_cid, V_RCID_CTRL);
    checkEq("on a fresh local id", c->local_cid, V_LCID_INTR);
  }

  /* An open channel is a different matter: it is in use, and handing it over
   * would drop traffic. That one is refused. */
  freshL2cap();
  openHid();
  sentCount = 0;
  closesSeen = 0;
  FEED(V_IN_CONN_REQ_CTRL);
  EXPECT("an open channel is not surrendered", 0, DS_L2CAP_CONN_RSP,
         V_OUT_CONN_RSP_NO_RESOURCES);
  check("and it stays open", ds_l2cap_is_open(&l2, DS_L2CAP_PSM_HID_CONTROL));
  check("nothing was closed", closesSeen == 0);
}

/* ------------------------------------------------------ config edge cases */

static void testConfigEdges() {
  Serial.println("configuration edge cases");

  /* An option with the hint bit set may be ignored; without it, refused. */
  freshL2cap();
  ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL);
  FEED(V_IN_CONN_RSP_CTRL);
  sentCount = 0;
  FEED(V_IN_CONFIG_REQ_HINT);
  EXPECT("unknown hint accepted", 0, DS_L2CAP_CONFIG_RSP,
         V_OUT_CONFIG_RSP_CTRL);

  freshL2cap();
  ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL);
  FEED(V_IN_CONN_RSP_CTRL);
  sentCount = 0;
  FEED(V_IN_CONFIG_REQ_RETRANSMIT);
  uint8_t id = EXPECT("retransmission mode refused", 0, DS_L2CAP_CONFIG_RSP,
                      V_OUT_CONFIG_RSP_UNKNOWN);
  checkEq("refusal echoes identifier", id, V_ID_UNKNOWN_OPT);
  check("channel not opened by a refused config",
        !ds_l2cap_is_open(&l2, DS_L2CAP_PSM_HID_CONTROL));

  /* They want a smaller MTU. Take it and ask once more. */
  freshL2cap();
  ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL);
  FEED(V_IN_CONN_RSP_CTRL);
  sentCount = 0;
  FEED(V_IN_CONFIG_RSP_SMALLER_MTU);
  EXPECT("retry with their mtu", 0, DS_L2CAP_CONFIG_REQ,
         V_OUT_CONFIG_REQ_RETRY);
  /* And only once: a second refusal ends it. */
  sentCount = 0;
  FEED(V_IN_CONFIG_RSP_SMALLER_MTU);
  check("no second retry", sentCount == 0);
  checkEq("channel closed", closesSeen, 1);

  /* A flat rejection closes immediately. */
  freshL2cap();
  ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL);
  FEED(V_IN_CONN_RSP_CTRL);
  closesSeen = 0;
  FEED(V_IN_CONFIG_RSP_REJECTED);
  checkEq("rejected config closes the channel", closesSeen, 1);

  /* Configuring a channel that does not exist. */
  freshL2cap();
  sentCount = 0;
  FEED(V_IN_CONFIG_REQ_BAD_CID);
  id = EXPECT("invalid cid rejected", 0, DS_L2CAP_CMD_REJECT,
              V_OUT_REJECT_INVALID_CID);
  checkEq("reject echoes identifier", id, V_ID_BAD_CID);
}

static void testConnectionRefused() {
  Serial.println("connection refused");

  freshL2cap();
  ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL);
  sentCount = 0;
  FEED(V_IN_CONN_RSP_PENDING);
  check("pending sends nothing", sentCount == 0);
  check("pending does not close", closesSeen == 0);
  /* ...and the real answer still lands. */
  FEED(V_IN_CONN_RSP_CTRL);
  EXPECT("config after pending", 0, DS_L2CAP_CONFIG_REQ,
         V_OUT_CONFIG_REQ_CTRL);

  freshL2cap();
  ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL);
  FEED(V_IN_CONN_RSP_REFUSED);
  checkEq("refusal closes the channel", closesSeen, 1);
  checkEq("closed psm reported", lastClosePsm, DS_L2CAP_PSM_HID_CONTROL);
  check("nothing is open", !ds_l2cap_is_open(&l2, DS_L2CAP_PSM_HID_CONTROL));
  /* A refused control channel must not drag the interrupt one up with it. */
  check("interrupt not attempted",
        !ds_l2cap_is_open(&l2, DS_L2CAP_PSM_HID_INTERRUPT));
}

/* -------------------------------------------------------------- teardown */

static void testTeardown() {
  Serial.println("teardown");
  freshL2cap();
  openHid();

  sentCount = 0;
  closesSeen = 0;
  FEED(V_IN_DISCONN_REQ);
  uint8_t id = EXPECT("disconnection response", 0, DS_L2CAP_DISCONN_RSP,
                      V_OUT_DISCONN_RSP);
  checkEq("response echoes identifier", id, V_ID_DISCONN_REQ);
  checkEq("one channel closed", closesSeen, 1);
  checkEq("the control one", lastClosePsm, DS_L2CAP_PSM_HID_CONTROL);
  check("interrupt survives", ds_l2cap_is_open(&l2,
                                                DS_L2CAP_PSM_HID_INTERRUPT));

  /* Our own disconnection. */
  freshL2cap();
  openHid();
  sentCount = 0;
  check("disconnect", ds_l2cap_disconnect(&l2, DS_L2CAP_PSM_HID_CONTROL));
  EXPECT("disconnection request", 0, DS_L2CAP_DISCONN_REQ, V_OUT_DISCONN_REQ);
  closesSeen = 0;
  FEED(V_IN_DISCONN_RSP);
  checkEq("closed on their response", closesSeen, 1);
  check("disconnecting a psm we never had",
        !ds_l2cap_disconnect(&l2, DS_L2CAP_PSM_SDP));

  /* The ACL link dropping takes everything with it, silently. */
  freshL2cap();
  openHid();
  sentCount = 0;
  closesSeen = 0;
  ds_l2cap_reset(&l2);
  checkEq("reset closes both", closesSeen, 2);
  check("reset says nothing on the wire", sentCount == 0);
  check("nothing open after reset", !ds_l2cap_hid_ready(&l2));
  /* And the object is reusable. */
  sentCount = 0;
  check("can connect again", ds_l2cap_connect_hid(&l2));
  EXPECT("cids start over", 0, DS_L2CAP_CONN_REQ, V_OUT_CONN_REQ_CTRL);
}

/* ------------------------------------------------------------ signalling */

static void testSignalling() {
  Serial.println("signalling odds and ends");
  freshL2cap();

  sentCount = 0;
  FEED(V_IN_ECHO_REQ);
  uint8_t id = EXPECT("echo answered", 0, DS_L2CAP_ECHO_RSP, V_OUT_ECHO_BODY);
  checkEq("echo echoes identifier", id, V_ID_ECHO);

  sentCount = 0;
  FEED(V_IN_INFO_REQ);
  id = EXPECT("information request answered", 0, DS_L2CAP_INFO_RSP,
              V_OUT_INFO_RSP);
  checkEq("info echoes identifier", id, V_ID_INFO);

  sentCount = 0;
  FEED(V_IN_UNKNOWN_CODE);
  id = EXPECT("unknown command rejected", 0, DS_L2CAP_CMD_REJECT,
              V_OUT_REJECT_NOT_UNDERSTOOD);
  checkEq("rejection echoes identifier", id, V_ID_UNKNOWN_CODE);

  /* Two commands in one packet: reading only the first loses the second. */
  sentCount = 0;
  FEED(V_IN_TWO_COMMANDS);
  checkEq("both commands answered", sentCount, 2);
  EXPECT("first is the echo", 0, DS_L2CAP_ECHO_RSP, V_OUT_ECHO_BODY);
  EXPECT("second is the info response", 1, DS_L2CAP_INFO_RSP, V_OUT_INFO_RSP);

  /* A truncated packet must be dropped, not read past. */
  sentCount = 0;
  ds_l2cap_handle(&l2, DS_L2CAP_CID_SIGNALLING, V_IN_ECHO_REQ, 3);
  check("runt signalling packet ignored", sentCount == 0);
  ds_l2cap_handle(&l2, DS_L2CAP_CID_SIGNALLING, V_IN_ECHO_REQ,
                   sizeof(V_IN_ECHO_REQ) - 1);
  check("packet shorter than its own length ignored", sentCount == 0);
}

static void testLimits() {
  Serial.println("limits");
  freshL2cap();

  check("two channels fit", ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL));
  check("second channel", ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_INTERRUPT));
  /* Three now: the two HID channels and the service discovery one a pad
   * opens on its way in. A fourth has nowhere to go. */
  check("service discovery fits too",
        ds_l2cap_connect(&l2, DS_L2CAP_PSM_SDP));
  check("a fourth has nowhere to go", !ds_l2cap_connect(&l2, 0x0015));
  check("the same psm twice is refused",
        !ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL));

  /* If the wire refuses the request there is no half-open channel left. */
  freshL2cap();
  sendFails = true;
  check("connect fails when the send does",
        !ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL));
  sendFails = false;
  check("and the slot was released",
        ds_l2cap_connect(&l2, DS_L2CAP_PSM_HID_CONTROL));

  ds_l2cap_config_t bad;
  memset(&bad, 0, sizeof(bad));
  ds_l2cap_t tmp;
  check("init without a send function refused", !ds_l2cap_init(&tmp, &bad));
}

/* ------------------------------------------------------------------- main */

/*
 * A pad that pages the host opens a channel to PSM 1 and asks what is here
 * before it authenticates. It does not read the answer -- BTstack registers
 * no records at all and the pad is satisfied -- but it does insist on a
 * well-formed one, and waits thirty seconds for it before giving up on the
 * link entirely.
 *
 * Everything in SDP is big endian, unlike the rest of Bluetooth.
 */
static void testSdp() {
  uint8_t rsp[DS_SDP_RSP_MAX];
  uint16_t n;

  Serial.println("service discovery");

  /* The query the pad actually sends, from the trace: search for a service
   * class and ask for a range of attributes. Answered with no records. */
  {
    static const uint8_t req[] = {0x06, 0x00, 0x01, 0x00, 0x0F, 0x35, 0x03,
                                  0x19, 0x01, 0x00, 0x08, 0x00, 0x35, 0x05,
                                  0x0A, 0x00, 0x00, 0xFF, 0xFF, 0x00};
    /* pdu 7, tid echoed, 5 bytes: two of list, an empty sequence, no
     * continuation. */
    static const uint8_t want[] = {0x07, 0x00, 0x01, 0x00, 0x05,
                                   0x00, 0x02, 0x35, 0x00, 0x00};
    n = ds_sdp_respond(req, sizeof(req), rsp, sizeof(rsp));
    checkEq("search-attribute answered", n, sizeof(want));
    checkBytes("search-attribute bytes", rsp, want, sizeof(want));
  }

  /* A plain service search: counts of zero and an empty handle list. */
  {
    static const uint8_t req[] = {0x02, 0xAB, 0xCD, 0x00, 0x08, 0x35, 0x03,
                                  0x19, 0x01, 0x00, 0xFF, 0xFF, 0x00};
    static const uint8_t want[] = {0x03, 0xAB, 0xCD, 0x00, 0x05,
                                   0x00, 0x00, 0x00, 0x00, 0x00};
    n = ds_sdp_respond(req, sizeof(req), rsp, sizeof(rsp));
    checkEq("service search answered", n, sizeof(want));
    checkBytes("service search bytes", rsp, want, sizeof(want));
  }

  /* Attributes of one record, which there are none of either. */
  {
    static const uint8_t req[] = {0x04, 0x00, 0x02, 0x00, 0x0D, 0x00, 0x01,
                                  0x00, 0x00, 0x00, 0xFF, 0x35, 0x03, 0x09,
                                  0x00, 0x00, 0x00};
    static const uint8_t want[] = {0x05, 0x00, 0x02, 0x00, 0x05,
                                   0x00, 0x02, 0x35, 0x00, 0x00};
    n = ds_sdp_respond(req, sizeof(req), rsp, sizeof(rsp));
    checkEq("attribute request answered", n, sizeof(want));
    checkBytes("attribute bytes", rsp, want, sizeof(want));
  }

  /* Anything else earns an error rather than silence. */
  {
    static const uint8_t req[] = {0x7F, 0x12, 0x34, 0x00, 0x00};
    static const uint8_t want[] = {0x01, 0x12, 0x34, 0x00, 0x02, 0x00, 0x03};
    n = ds_sdp_respond(req, sizeof(req), rsp, sizeof(rsp));
    checkEq("unknown pdu refused", n, sizeof(want));
    checkBytes("error bytes", rsp, want, sizeof(want));
  }

  /* Too short to carry a transaction id: there is nothing to answer and
   * nowhere to send it. */
  {
    static const uint8_t req[] = {0x06, 0x00};
    checkEq("runt ignored", ds_sdp_respond(req, sizeof(req), rsp,
                                            sizeof(rsp)), 0);
  }
}

static void runAll() {
  passCount = failCount = 0;
  Serial.println();
  Serial.println("=== DualSense32 L2CAP self-test ===");
  testHandshake();
  testTraffic();
  testInboundOpen();
  testOpenCollision();
  testConfigEdges();
  testConnectionRefused();
  testTeardown();
  testSignalling();
  testLimits();
  testSdp();
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

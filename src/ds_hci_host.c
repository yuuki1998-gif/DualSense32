/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_hci_host.c — the HCI conversation. See ds_hci_host.h.
 *
 * Depends on ds_hci.c and string.h and nothing else, so the whole
 * connect-and-pair sequence can be exercised on a desk with the wire replaced
 * by a recording function.
 */
#include "ds_hci_host.h"

#include <string.h>

#define DEFAULT_NAME    "DualSense32"
#define DEFAULT_COD     0x000104UL   /* computer, handheld */
#define ERR_REMOTE_USER 0x13         /* "remote user terminated connection" */
#define ERR_UNACCEPTABLE_ADDR 0x0F   /* "rejected due to unacceptable BD_ADDR" */

/* ========================================================================== */
/*                                plumbing                                    */
/* ========================================================================== */

static void say(ds_hci_host_t *h, const char *msg) {
  if (h->cfg.log != NULL) h->cfg.log(h->cfg.ctx, msg);
}

static void set_state(ds_hci_host_t *h, ds_hci_host_state_t st) {
  if (h->state == st) return;
  h->state = st;
  if (h->cfg.on_state != NULL) h->cfg.on_state(h->cfg.ctx, st);
}

/* Sends an already-built command and remembers its opcode so the matching
 * Command Complete can be recognised. `len` of 0 means the builder refused,
 * which is a programming error rather than a runtime one. */
static bool emit(ds_hci_host_t *h, uint16_t len) {
  if (len == 0) {
    say(h, "hci: command builder refused");
    set_state(h, DS_HOST_FAILED);
    return false;
  }
  h->pending_opcode = (uint16_t)(h->tx[0] | ((uint16_t)h->tx[1] << 8));
  if (h->cfg.send_cmd(h->cfg.ctx, h->tx, len) != 0) {
    say(h, "hci: send failed");
    set_state(h, DS_HOST_FAILED);
    return false;
  }
  return true;
}

static void fail(ds_hci_host_t *h, const char *why) {
  say(h, why);
  if (h->have_handle) {
    /* Giving up here without saying so leaves the link alive at the
     * controller. A retry that pages the same address right after gets ACL
     * Connection Already Exists (0x0B) and fails the same way, a few
     * milliseconds later -- traced to exactly this. Best-effort: the failure
     * that brought us here may already have taken the transport with it. */
    emit(h, ds_hci_cmd_disconnect(h->tx, sizeof(h->tx), h->handle,
                                   ERR_REMOTE_USER));
  }
  h->have_handle = false;
  h->encrypted = false;
  /* Mirrors on_disconn_complete, and for the same reason: the Disconnection
   * Complete that follows the line above will find have_handle already false
   * and be ignored there. This is therefore the only place some failures --
   * encryption dropping, a key the pad no longer agrees with, after it has
   * already done its SDP -- ever get their credits back. Left stuck instead,
   * they erode a little on every such failure until sends start being
   * refused for a pad that is otherwise fine. */
  h->acl_credits = h->acl_credits_total;
  ds_hci_reasm_reset(&h->reasm);
  set_state(h, DS_HOST_FAILED);
}

/* A link can be started from either of these; FAILED is a resting state, not
 * a latch. */
static bool link_idle(const ds_hci_host_t *h) {
  return h->state == DS_HOST_READY || h->state == DS_HOST_FAILED ||
         h->state == DS_HOST_INQUIRING;
}

/* ========================================================================== */
/*                            setup sequence                                  */
/* ========================================================================== */

/* Order matters: Reset first because a freshly plugged dongle may be mid
 * anything, buffer size before any ACL is sent, and simple pairing mode
 * before scanning is enabled so we never advertise a mode we cannot honour. */
enum {
  STEP_RESET = 0,
  STEP_READ_VERSION,
  STEP_READ_LOCAL_COMMANDS,
  STEP_READ_LOCAL_FEATURES,
  STEP_READ_BUFFER_SIZE,
  STEP_READ_BD_ADDR,
  STEP_EVENT_MASK,
  STEP_EVENT_MASK_PAGE_2,
  STEP_LINK_POLICY,
  STEP_SIMPLE_PAIRING,
  STEP_SECURE_CONNECTIONS,
  STEP_INQUIRY_MODE,
  STEP_CLASS_OF_DEVICE,
  STEP_LOCAL_NAME,
  STEP_PAGE_TIMEOUT,
  STEP_PAGE_SCAN_ACTIVITY,
  STEP_PAGE_SCAN_TYPE,
  STEP_SCAN_ENABLE,
  STEP_COUNT
};

/*
 * A controller is allowed not to implement these, and one that does not is
 * still a perfectly good radio for a gamepad. Everything else is load-bearing:
 * failing there means the dongle is not usable and saying so is the point.
 */
static bool step_is_optional(uint8_t step) {
  switch (step) {
    case STEP_READ_LOCAL_COMMANDS:
    case STEP_READ_LOCAL_FEATURES:
    case STEP_EVENT_MASK_PAGE_2:
    case STEP_SECURE_CONNECTIONS:
    case STEP_INQUIRY_MODE:
    case STEP_PAGE_TIMEOUT:
    case STEP_PAGE_SCAN_ACTIVITY:
    case STEP_PAGE_SCAN_TYPE:
      return true;
    default:
      return false;
  }
}

static uint16_t build_step(ds_hci_host_t *h, uint8_t step) {
  uint8_t *b = h->tx;
  uint16_t cap = (uint16_t)sizeof(h->tx);

  switch (step) {
    case STEP_RESET:
      return ds_hci_cmd_reset(b, cap);
    case STEP_READ_VERSION:
      return ds_hci_cmd(b, cap, DS_HCI_READ_LOCAL_VERSION, NULL, 0);
    case STEP_READ_BUFFER_SIZE:
      return ds_hci_cmd(b, cap, DS_HCI_READ_BUFFER_SIZE, NULL, 0);
    case STEP_READ_BD_ADDR:
      return ds_hci_cmd(b, cap, DS_HCI_READ_BD_ADDR, NULL, 0);
    case STEP_EVENT_MASK:
      /* All ones. The spec default masks off the secure simple pairing
       * events, and without those a modern pairing silently stalls. */
      return ds_hci_cmd_set_event_mask(b, cap, 0xFFFFFFFFFFFFFFFFULL);
    case STEP_READ_LOCAL_COMMANDS:
      return ds_hci_cmd(b, cap, DS_HCI_READ_LOCAL_COMMANDS, NULL, 0);
    case STEP_READ_LOCAL_FEATURES:
      return ds_hci_cmd(b, cap, DS_HCI_READ_LOCAL_FEATURES, NULL, 0);
    case STEP_EVENT_MASK_PAGE_2: {
      /* Page 2 defaults to all zeroes, which masks off the events a modern
       * controller reports about secure connections and flow specification.
       * All ones, for the same reason page 1 is. */
      uint8_t p[8];
      memset(p, 0xFF, sizeof(p));
      return ds_hci_cmd(b, cap, DS_HCI_SET_EVENT_MASK_PAGE_2, p, sizeof(p));
    }
    case STEP_SECURE_CONNECTIONS: {
      /* Without this the pad and this host negotiate legacy pairing and E0
       * encryption, which is not what a PlayStation does with the same pad. */
      uint8_t p = 0x01;
      return ds_hci_cmd(b, cap, DS_HCI_WRITE_SECURE_CONN_HOST, &p, 1);
    }
    case STEP_INQUIRY_MODE: {
      /* Results with RSSI and extended inquiry response rather than the bare
       * form; the extra fields are how a gamepad is told apart from a
       * headset without connecting to it first. */
      uint8_t p = 0x02;
      return ds_hci_cmd(b, cap, DS_HCI_WRITE_INQUIRY_MODE, &p, 1);
    }
    case STEP_PAGE_TIMEOUT: {
      uint8_t p[2];
      p[0] = (uint8_t)(DS_HCI_PAGE_TIMEOUT_DEFAULT & 0xFF);
      p[1] = (uint8_t)(DS_HCI_PAGE_TIMEOUT_DEFAULT >> 8);
      return ds_hci_cmd(b, cap, DS_HCI_WRITE_PAGE_TIMEOUT, p, sizeof(p));
    }
    case STEP_PAGE_SCAN_ACTIVITY: {
      uint8_t p[4];
      p[0] = (uint8_t)(DS_HCI_PAGE_SCAN_INTERVAL_DEFAULT & 0xFF);
      p[1] = (uint8_t)(DS_HCI_PAGE_SCAN_INTERVAL_DEFAULT >> 8);
      p[2] = (uint8_t)(DS_HCI_PAGE_SCAN_WINDOW_DEFAULT & 0xFF);
      p[3] = (uint8_t)(DS_HCI_PAGE_SCAN_WINDOW_DEFAULT >> 8);
      return ds_hci_cmd(b, cap, DS_HCI_WRITE_PAGE_SCAN_ACTIVITY, p, sizeof(p));
    }
    case STEP_PAGE_SCAN_TYPE: {
      uint8_t p = DS_HCI_PAGE_SCAN_TYPE_INTERLACED;
      return ds_hci_cmd(b, cap, DS_HCI_WRITE_PAGE_SCAN_TYPE, &p, 1);
    }
    case STEP_LINK_POLICY: {
      /* Out of reset a controller allows none of these, so every sniff or
       * role-switch request from the peer is refused. Saying yes up front is
       * what a normal host does and costs nothing. */
      uint8_t p[2];
      p[0] = (uint8_t)(DS_HCI_POLICY_DEFAULT & 0xFF);
      p[1] = (uint8_t)(DS_HCI_POLICY_DEFAULT >> 8);
      return ds_hci_cmd(b, cap, DS_HCI_WRITE_DEFAULT_LINK_POLICY, p,
                         sizeof(p));
    }
    case STEP_CLASS_OF_DEVICE:
      return ds_hci_cmd_write_class_of_device(b, cap, h->cfg.class_of_device);
    case STEP_LOCAL_NAME:
      return ds_hci_cmd_write_local_name(b, cap, h->cfg.name);
    case STEP_SIMPLE_PAIRING:
      return ds_hci_cmd_write_simple_pairing_mode(b, cap, true);
    case STEP_SCAN_ENABLE:
      return ds_hci_cmd_write_scan_enable(b, cap, h->cfg.scan_enable);
    default:
      return 0;
  }
}

static void capture_step(ds_hci_host_t *h, uint8_t step,
                         const uint8_t *ret, uint8_t ret_len) {
  switch (step) {
    case STEP_READ_VERSION:
      /* status, HCI_Version, HCI_Revision(2), LMP_Version,
       * Manufacturer_Name(2), LMP_Subversion(2) */
      if (ret_len < 9) return;
      h->hci_version = ret[1];
      h->manufacturer = (uint16_t)(ret[5] | ((uint16_t)ret[6] << 8));
      break;
    case STEP_READ_BUFFER_SIZE:
      /* status, ACL_Data_Packet_Length(2), SCO length, Total_Num_ACL(2), ... */
      if (ret_len < 7) return;
      h->acl_mtu = (uint16_t)(ret[1] | ((uint16_t)ret[2] << 8));
      h->acl_credits_total = (uint16_t)(ret[4] | ((uint16_t)ret[5] << 8));
      h->acl_credits = h->acl_credits_total;
      break;
    case STEP_READ_BD_ADDR:
      if (ret_len < 7) return;
      memcpy(h->local_addr, ret + 1, DS_HCI_BD_ADDR_LEN);
      break;
    default:
      break;
  }
}

static void advance_setup(ds_hci_host_t *h) {
  h->init_step++;
  if (h->init_step >= STEP_COUNT) {
    say(h, "hci: controller ready");
    set_state(h, DS_HOST_READY);
    return;
  }
  emit(h, build_step(h, h->init_step));
}

/* ========================================================================== */
/*                              event handlers                                */
/* ========================================================================== */

static void on_cmd_complete(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  ds_hci_cmd_complete_t cc;
  if (!ds_hci_parse_cmd_complete(ev, &cc)) return;

  if (h->state != DS_HOST_INITIALISING) return;
  if (cc.opcode != h->pending_opcode) return;

  if (cc.status != DS_HCI_SUCCESS) {
    if (!step_is_optional(h->init_step)) {
      fail(h, "hci: setup command rejected");
      return;
    }
    say(h, "hci: controller declined an optional step");
    advance_setup(h);
    return;
  }
  capture_step(h, h->init_step, cc.ret, cc.ret_len);
  advance_setup(h);
}

static void on_cmd_status(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  ds_hci_cmd_status_t cs;
  if (!ds_hci_parse_cmd_status(ev, &cs)) return;
  if (cs.status == DS_HCI_SUCCESS) return;

  /* Command Status only carries a failure worth acting on for the commands
   * that would otherwise be answered by a later event that now never comes. */
  if (cs.opcode == DS_HCI_CREATE_CONNECTION) {
    fail(h, "hci: page refused");
  } else if (cs.opcode == DS_HCI_AUTHENTICATION_REQUESTED ||
             cs.opcode == DS_HCI_SET_CONNECTION_ENCRYPTION) {
    fail(h, "hci: authentication refused");
  } else if (cs.opcode == DS_HCI_INQUIRY && h->state == DS_HOST_INQUIRING) {
    set_state(h, DS_HOST_READY);
  }
}

static void on_conn_complete(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  ds_hci_conn_complete_t c;
  if (!ds_hci_parse_conn_complete(ev, &c)) return;

  h->last_conn_status = c.status;
  if (c.status != DS_HCI_SUCCESS) {
    fail(h, ds_hci_error_name(c.status));
    return;
  }
  h->handle = c.handle;
  h->have_handle = true;
  h->encrypted = false;
  h->authenticating = false;
  /* is_central is deliberately not touched here. Connection Complete arrives
   * after the Role Change that answers the question, so initialising it at
   * this point overwrites the answer with a guess. */
  memcpy(h->peer_addr, c.addr, DS_HCI_BD_ADDR_LEN);
  ds_hci_reasm_reset(&h->reasm);

  /* Always from this end, whoever paged. A DualSense that calls in is
   * completely passive: it authenticates nothing, encrypts nothing and opens
   * no channels. Waiting for it to lead means waiting for ever — measured,
   * after trying it.
   *
   * Note what a stored link key means here. The connection straight after
   * pairing is already authenticated, so the key is never used; the first
   * time it is tried is the first reconnection. A wrong one therefore looks
   * like pairing working and then never working again. */
  /* Find out what the peer can do before deciding anything with it. The
   * answers arrive as events; authentication starts from whichever of them
   * comes back, so a controller that refuses still gets there. */
  say(h, "hci: acl up, reading remote features");
  emit(h, ds_hci_cmd_handle(h->tx, sizeof(h->tx),
                             DS_HCI_READ_REMOTE_FEATURES, c.handle));
}

static void on_conn_request(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  ds_hci_conn_request_t r;
  if (!ds_hci_parse_conn_request(ev, &r)) return;

  /* Before the idle check, so a stranger is told no even while we are busy
   * paging, instead of being left for the controller's accept timeout. The
   * command goes out on its own; the state and the peer are not touched. */
  if (h->cfg.accept_peer != NULL && !h->cfg.accept_peer(h->cfg.ctx, r.addr)) {
    uint8_t p[DS_HCI_BD_ADDR_LEN + 1];
    memcpy(p, r.addr, DS_HCI_BD_ADDR_LEN);
    p[DS_HCI_BD_ADDR_LEN] = ERR_UNACCEPTABLE_ADDR;
    say(h, "hci: caller is not the bonded pad, rejecting");
    emit(h, ds_hci_cmd(h->tx, sizeof(h->tx), DS_HCI_REJECT_CONNECTION_REQUEST,
                       p, sizeof(p)));
    return;
  }
  if (!link_idle(h)) return;   /* already busy; let it time out on their side */

  memcpy(h->peer_addr, r.addr, DS_HCI_BD_ADDR_LEN);
  h->inbound = true;
  h->is_central = false;   /* until the switch asked for below succeeds */
  set_state(h, DS_HOST_CONNECTING);
  say(h, "hci: incoming connection, accepting");
  /*
   * Role 0x01: stay the peripheral. This was briefly 0x00 on the theory that
   * the five second supervision timeout a paging DualSense sets was killing
   * the link. Traced against BTstack on the same pad and dongle, which holds
   * the link for ninety seconds: it accepts with 0x01, never writes the
   * supervision timeout, and runs at the pad's five seconds throughout. The
   * role was never the problem.
   */
  emit(h, ds_hci_cmd_accept_connection(h->tx, sizeof(h->tx), r.addr, 0x01));
}

/* Read Remote Supported Features Complete: status, handle, 8 bytes of LMP
 * features. Page 1 of the extended set is where secure connections and sniff
 * subrating live, so ask for that too and authenticate after it. */
static void on_remote_features(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  if (!h->have_handle) return;
  emit(h, ds_hci_cmd_read_remote_ext_features(h->tx, sizeof(h->tx),
                                               h->handle, 1));
}

static void start_authentication(ds_hci_host_t *h) {
  if (!h->have_handle || h->authenticating) return;
  /*
   * A pad that called in is in the middle of its own sequence: it opens a
   * service discovery channel, asks its question, closes it, and only then
   * asks for the link key. Interrupting that with a request of ours leaves
   * the discovery unfinished, and thirty seconds later the pad gives up on
   * the link and pages again. Let it lead; the Link Key Request it sends
   * arrives on its own and takes the connection the rest of the way.
   */
  if (h->inbound) {
    say(h, "hci: waiting for the pad to authenticate");
    return;
  }
  h->authenticating = true;
  say(h, "hci: authenticating");
  emit(h, ds_hci_cmd_authentication_requested(h->tx, sizeof(h->tx),
                                               h->handle));
}

/* Role Change: status, bd_addr(6), new role. 0x00 is central. */
static void on_role_change(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  if (ev->plen < 8) return;
  if (ev->params[0] != DS_HCI_SUCCESS) {
    say(h, "hci: stayed peripheral");
    return;
  }
  h->is_central = ev->params[7] == 0x00;
  say(h, h->is_central ? "hci: now central" : "hci: now peripheral");
}

static void on_disconn_complete(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  ds_hci_disconn_complete_t d;
  if (!ds_hci_parse_disconn_complete(ev, &d)) return;
  if (!h->have_handle || d.handle != h->handle) return;

  h->have_handle = false;
  h->encrypted = false;
  /* The spec has the host treat every packet still queued for this handle as
   * flushed, and no Number Of Completed Packets will come for them. A pad
   * that goes silent leaves the controller's buffers full, so without this
   * the next PS-button connection starts with none, cannot answer the pad's
   * service discovery, and is dropped thirty seconds later. One link at a
   * time, so every credit is ours again. */
  h->acl_credits = h->acl_credits_total;
  h->last_disconnect_reason = d.reason;
  ds_hci_reasm_reset(&h->reasm);
  say(h, ds_hci_error_name(d.reason));
  set_state(h, DS_HOST_READY);
}

static void on_link_key_request(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  uint8_t addr[DS_HCI_BD_ADDR_LEN];
  uint8_t key[DS_HCI_LINK_KEY_LEN];

  if (!ds_hci_parse_addr_event(ev, addr)) return;

  if (h->cfg.get_link_key != NULL &&
      h->cfg.get_link_key(h->cfg.ctx, addr, key)) {
    say(h, "hci: reusing stored link key");
    emit(h, ds_hci_cmd_link_key_reply(h->tx, sizeof(h->tx), addr, key));
    return;
  }
  /* No key means pair from scratch. Refusing here is what triggers secure
   * simple pairing rather than an error. */
  say(h, "hci: no stored key, pairing");
  emit(h, ds_hci_cmd_link_key_neg_reply(h->tx, sizeof(h->tx), addr));
}

static void on_io_capability_request(ds_hci_host_t *h,
                                     const ds_hci_event_t *ev) {
  uint8_t addr[DS_HCI_BD_ADDR_LEN];
  if (!ds_hci_parse_addr_event(ev, addr)) return;

  emit(h, ds_hci_cmd_io_capability_reply(h->tx, sizeof(h->tx), addr,
                                          h->cfg.io_capability, 0x00,
                                          h->cfg.auth_requirements));
}

static void on_user_confirmation(ds_hci_host_t *h,
                                 const ds_hci_event_t *ev) {
  ds_hci_user_confirmation_t uc;
  if (!ds_hci_parse_user_confirmation(ev, &uc)) return;

  /* With no display and no keypad on either side this is Just Works: the
   * numeric value is not shown to anybody, and accepting is the only sensible
   * answer. There is no man in the middle protection in that mode, which is
   * the accepted trade for a gamepad. */
  emit(h, ds_hci_cmd_user_confirmation_reply(h->tx, sizeof(h->tx), uc.addr,
                                              true));
}

static void on_link_key_notification(ds_hci_host_t *h,
                                     const ds_hci_event_t *ev) {
  ds_hci_link_key_notification_t n;
  if (!ds_hci_parse_link_key_notification(ev, &n)) return;

  h->last_key_type = n.key_type;
  say(h, n.key_type == 0x06 ? "hci: link key replaced"
                            : "hci: new link key");
  if (h->cfg.on_link_key != NULL) {
    h->cfg.on_link_key(h->cfg.ctx, n.addr, n.key);
  }
}

static void on_auth_complete(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  /* Status(1) Connection_Handle(2) */
  if (ev->plen < 3) return;
  h->last_auth_status = ev->params[0];
  if (ev->params[0] != DS_HCI_SUCCESS) {
    /* 0x05 is the keys disagreeing; 0x06 is the peer having no key for us,
     * which means the pad forgot this host and only pairing mode fixes it. */
    fail(h, ds_hci_error_name(ev->params[0]));
    return;
  }
  say(h, "hci: authenticated, encrypting");
  emit(h, ds_hci_cmd_set_connection_encryption(h->tx, sizeof(h->tx),
                                                h->handle, true));
}

static void on_encryption_change(ds_hci_host_t *h,
                                 const ds_hci_event_t *ev) {
  /* Status(1) Connection_Handle(2) Encryption_Enabled(1) */
  if (ev->plen < 4) return;
  if (ev->params[0] != DS_HCI_SUCCESS) {
    fail(h, "hci: encryption failed");
    return;
  }
  h->encrypted = ev->params[3] != 0;
  if (!h->encrypted) {
    fail(h, "hci: encryption turned off");
    return;
  }
  /* Fire and forget: the answer is only ever interesting in a log, and the
   * command complete for it is ignored because setup is long finished. */
  emit(h, ds_hci_cmd_handle(h->tx, sizeof(h->tx),
                             DS_HCI_READ_ENCRYPTION_KEY_SIZE, h->handle));
  say(h, "hci: connected");
  set_state(h, DS_HOST_CONNECTED);
}

/* The controller tells us how many of its buffers it has drained. Without
 * putting these credits back we would send exactly acl_credits_total packets
 * and then go quiet for ever. */
static void on_num_completed(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  uint8_t n, i;
  if (ev->plen < 1) return;

  n = ev->params[0];
  if ((uint32_t)1 + (uint32_t)n * 4 > ev->plen) return;

  for (i = 0; i < n; i++) {
    const uint8_t *e = ev->params + 1 + (size_t)i * 4;
    uint16_t handle = (uint16_t)((e[0] | ((uint16_t)e[1] << 8)) & 0x0FFF);
    uint16_t done = (uint16_t)(e[2] | ((uint16_t)e[3] << 8));
    if (!h->have_handle || handle != h->handle) continue;
    h->acl_credits = (uint16_t)(h->acl_credits + done);
    if (h->acl_credits > h->acl_credits_total) {
      h->acl_credits = h->acl_credits_total;
    }
  }
}

static void on_inquiry_result(ds_hci_host_t *h, const ds_hci_event_t *ev) {
  uint8_t n = ds_hci_inquiry_result_count(ev);
  uint8_t i;

  if (h->cfg.on_device_found == NULL) return;
  for (i = 0; i < n; i++) {
    ds_hci_inquiry_result_t r;
    if (!ds_hci_parse_inquiry_result(ev, i, &r)) break;
    h->cfg.on_device_found(h->cfg.ctx, r.addr, r.class_of_device, r.rssi,
                           r.has_rssi);
  }
}

void ds_hci_host_handle_event(ds_hci_host_t *h, const uint8_t *pkt,
                               uint16_t len) {
  ds_hci_event_t ev;
  if (h == NULL || !ds_hci_parse_event(pkt, len, &ev)) return;

  switch (ev.code) {
    case DS_HCI_EVT_COMMAND_COMPLETE:        on_cmd_complete(h, &ev); break;
    case DS_HCI_EVT_COMMAND_STATUS:          on_cmd_status(h, &ev); break;
    case DS_HCI_EVT_CONNECTION_COMPLETE:     on_conn_complete(h, &ev); break;
    case DS_HCI_EVT_CONNECTION_REQUEST:      on_conn_request(h, &ev); break;
    case DS_HCI_EVT_DISCONNECTION_COMPLETE:  on_disconn_complete(h, &ev); break;
    case DS_HCI_EVT_LINK_KEY_REQUEST:        on_link_key_request(h, &ev); break;
    case DS_HCI_EVT_IO_CAPABILITY_REQUEST:   on_io_capability_request(h, &ev); break;
    case DS_HCI_EVT_USER_CONFIRMATION_REQUEST: on_user_confirmation(h, &ev); break;
    case DS_HCI_EVT_LINK_KEY_NOTIFICATION:   on_link_key_notification(h, &ev); break;
    case DS_HCI_EVT_AUTHENTICATION_COMPLETE: on_auth_complete(h, &ev); break;
    case DS_HCI_EVT_ENCRYPTION_CHANGE:       on_encryption_change(h, &ev); break;
    case DS_HCI_EVT_NUM_COMPLETED_PACKETS:   on_num_completed(h, &ev); break;
    case DS_HCI_EVT_REMOTE_FEATURES_COMPLETE: on_remote_features(h, &ev); break;
    case DS_HCI_EVT_REMOTE_EXT_FEATURES:     start_authentication(h); break;
    case DS_HCI_EVT_ROLE_CHANGE:             on_role_change(h, &ev); break;
    case DS_HCI_EVT_INQUIRY_RESULT:
    case DS_HCI_EVT_INQUIRY_RESULT_RSSI:
    case DS_HCI_EVT_EXTENDED_INQUIRY_RESULT: on_inquiry_result(h, &ev); break;
    case DS_HCI_EVT_INQUIRY_COMPLETE:
      if (h->state == DS_HOST_INQUIRING) set_state(h, DS_HOST_READY);
      break;
    default: {
      /* Not acted on, but not thrown away either: an event nobody expected is
       * the first sign that the peer wants something this host does not do.
       * The code goes out with the name, because "unknown" on its own has
       * exactly the information content of silence. */
      static const char hexd[] = "0123456789ABCDEF";
      static char line[48];
      const char *name = ds_hci_event_name(ev.code);
      uint8_t i = 0;
      line[i++] = 'e'; line[i++] = 'v'; line[i++] = 't'; line[i++] = ' ';
      line[i++] = '0'; line[i++] = 'x';
      line[i++] = hexd[(ev.code >> 4) & 0x0F];
      line[i++] = hexd[ev.code & 0x0F];
      line[i++] = ' ';
      while (*name != '\0' && i < sizeof(line) - 1) line[i++] = *name++;
      /* Event 0x38 is Connection_Handle(2) then the timeout in slots of
       * 0.625 ms — no status byte in front of it, unlike almost every other
       * event. A link that
       * dies of a supervision timeout is worth knowing the timeout of. */
      if (ev.code == 0x38 && ev.plen >= 4) {
        uint32_t slots = (uint32_t)(ev.params[2] | ((uint16_t)ev.params[3] << 8));
        uint32_t ms = slots * 5 / 8;
        if (i < sizeof(line) - 8) {
          line[i++] = ' ';
          line[i++] = (char)('0' + (ms / 10000) % 10);
          line[i++] = (char)('0' + (ms / 1000) % 10);
          line[i++] = (char)('0' + (ms / 100) % 10);
          line[i++] = (char)('0' + (ms / 10) % 10);
          line[i++] = (char)('0' + ms % 10);
          line[i++] = 'm'; line[i++] = 's';
        }
      }
      line[i] = '\0';
      say(h, line);
      break;
    }
  }
}

/* ========================================================================== */
/*                                  A C L                                     */
/* ========================================================================== */

void ds_hci_host_handle_acl(ds_hci_host_t *h, const uint8_t *pkt,
                             uint16_t len) {
  ds_hci_acl_t a;
  const uint8_t *whole = NULL;
  uint16_t whole_len = 0;

  if (h == NULL || !ds_hci_parse_acl(pkt, len, &a)) return;
  if (!h->have_handle || a.handle != h->handle) return;

  if (ds_hci_reasm_push(&h->reasm, &a, &whole, &whole_len) !=
      DS_HCI_REASM_DONE) {
    return;
  }
  /* whole is length(2) channel id(2) payload; hand up the useful half. */
  if (whole_len < 4 || h->cfg.on_l2cap == NULL) return;
  h->cfg.on_l2cap(h->cfg.ctx, (uint16_t)(whole[2] | ((uint16_t)whole[3] << 8)),
                  whole + 4, (uint16_t)(whole_len - 4));
}

/* Largest ACL payload we may put in one fragment. */
static uint16_t frag_limit(const ds_hci_host_t *h) {
  uint16_t room = (uint16_t)(sizeof(h->tx) - DS_HCI_ACL_HDR_LEN);
  if (h->acl_mtu != 0 && h->acl_mtu < room) return h->acl_mtu;
  return room;
}

bool ds_hci_host_send_l2cap(ds_hci_host_t *h, uint16_t cid,
                             const uint8_t *data, uint16_t len) {
  uint16_t limit, first, sent, needed;

  /*
   * have_handle, not CONNECTED. The link is usable the moment Connection
   * Complete names it, and a pad that calls in asks its service discovery
   * question well before encryption -- so gating on CONNECTED silently threw
   * away every reply it was waiting for.
   */
  if (h == NULL || !h->have_handle) return false;
  if (h->state != DS_HOST_CONNECTED && h->state != DS_HOST_CONNECTING) {
    return false;
  }
  if (len != 0 && data == NULL) return false;

  limit = frag_limit(h);
  if (limit <= 4) return false;              /* no room for an L2CAP header */

  first = (uint16_t)(limit - 4);
  if (first > len) first = len;
  needed = 1;
  if (len > first) {
    needed = (uint16_t)(1 + ((len - first) + limit - 1) / limit);
  }
  if (needed > h->acl_credits) return false;

  /* First fragment: ACL header, L2CAP header, then as much payload as fits. */
  ds_hci_acl_header(h->tx, sizeof(h->tx), h->handle, DS_HCI_PB_FIRST_FLUSH,
                     (uint16_t)(4 + first));
  h->tx[4] = (uint8_t)(len & 0xFF);
  h->tx[5] = (uint8_t)(len >> 8);
  h->tx[6] = (uint8_t)(cid & 0xFF);
  h->tx[7] = (uint8_t)(cid >> 8);
  if (first != 0) memcpy(h->tx + 8, data, first);
  if (h->cfg.send_acl(h->cfg.ctx, h->tx, (uint16_t)(8 + first)) != 0) {
    return false;
  }
  h->acl_credits--;

  for (sent = first; sent < len; ) {
    uint16_t chunk = (uint16_t)(len - sent);
    if (chunk > limit) chunk = limit;
    ds_hci_acl_header(h->tx, sizeof(h->tx), h->handle, DS_HCI_PB_CONTINUING,
                       chunk);
    memcpy(h->tx + DS_HCI_ACL_HDR_LEN, data + sent, chunk);
    if (h->cfg.send_acl(h->cfg.ctx, h->tx,
                        (uint16_t)(DS_HCI_ACL_HDR_LEN + chunk)) != 0) {
      return false;
    }
    h->acl_credits--;
    sent = (uint16_t)(sent + chunk);
  }
  return true;
}

/* ========================================================================== */
/*                              public entry points                           */
/* ========================================================================== */

bool ds_hci_host_init(ds_hci_host_t *h, const ds_hci_host_config_t *cfg) {
  if (h == NULL || cfg == NULL) return false;
  if (cfg->send_cmd == NULL || cfg->send_acl == NULL) return false;

  memset(h, 0, sizeof(*h));
  h->cfg = *cfg;
  if (h->cfg.name == NULL) h->cfg.name = DEFAULT_NAME;
  if (h->cfg.class_of_device == 0) h->cfg.class_of_device = DEFAULT_COD;
  if (h->cfg.io_capability == 0) {
    h->cfg.io_capability = DS_HCI_IO_NO_INPUT_OUTPUT;
  }
  if (h->cfg.auth_requirements == 0) {
    h->cfg.auth_requirements = DS_HCI_AUTH_NO_MITM_GENERAL;
  }
  if (h->cfg.scan_enable == 0) h->cfg.scan_enable = DS_HCI_SCAN_PAGE;
  h->state = DS_HOST_IDLE;
  return true;
}

bool ds_hci_host_start(ds_hci_host_t *h) {
  if (h == NULL) return false;

  h->init_step = STEP_RESET;
  h->have_handle = false;
  h->encrypted = false;
  h->acl_credits = 0;
  ds_hci_reasm_reset(&h->reasm);
  set_state(h, DS_HOST_INITIALISING);
  return emit(h, build_step(h, STEP_RESET));
}

bool ds_hci_host_inquiry(ds_hci_host_t *h, uint8_t seconds) {
  uint8_t units;
  if (h == NULL || !link_idle(h)) return false;

  /* The controller counts in 1.28 s units, 1 to 48. */
  units = (uint8_t)((seconds * 100 + 64) / 128);
  if (units < 1) units = 1;
  if (units > 48) units = 48;

  set_state(h, DS_HOST_INQUIRING);
  return emit(h, ds_hci_cmd_inquiry(h->tx, sizeof(h->tx), DS_HCI_LAP_GIAC,
                                     units, 0));
}

bool ds_hci_host_inquiry_cancel(ds_hci_host_t *h) {
  if (h == NULL || h->state != DS_HOST_INQUIRING) return false;
  set_state(h, DS_HOST_READY);
  return emit(h, ds_hci_cmd(h->tx, sizeof(h->tx), DS_HCI_INQUIRY_CANCEL,
                             NULL, 0));
}

bool ds_hci_host_connect(ds_hci_host_t *h,
                          const uint8_t addr[DS_HCI_BD_ADDR_LEN]) {
  if (h == NULL || addr == NULL || !link_idle(h)) return false;

  if (h->state == DS_HOST_INQUIRING) {
    /* Paging during an inquiry is refused by most controllers, and the
     * Inquiry Complete we would otherwise wait for never arrives. */
    emit(h, ds_hci_cmd(h->tx, sizeof(h->tx), DS_HCI_INQUIRY_CANCEL, NULL, 0));
  }
  memcpy(h->peer_addr, addr, DS_HCI_BD_ADDR_LEN);
  h->inbound = false;
  set_state(h, DS_HOST_CONNECTING);
  h->is_central = true;   /* the pager is the central */
  say(h, "hci: paging");
  return emit(h, ds_hci_cmd_create_connection(h->tx, sizeof(h->tx), addr,
                                               DS_HCI_PKT_TYPE_DEFAULT, 0x01,
                                               0x0000, true));
}

bool ds_hci_host_disconnect(ds_hci_host_t *h) {
  if (h == NULL || !h->have_handle) return false;
  return emit(h, ds_hci_cmd_disconnect(h->tx, sizeof(h->tx), h->handle,
                                        ERR_REMOTE_USER));
}

/* ========================================================================== */
/*                                  helpers                                   */
/* ========================================================================== */

bool ds_hci_cod_is_gamepad(uint32_t cod) {
  uint8_t major = (uint8_t)((cod >> 8) & 0x1F);
  uint8_t minor = (uint8_t)((cod >> 2) & 0x3F);
  /* Major 5 is Peripheral. Within that the low four bits of the minor field
   * name the device; 2 is a gamepad. The top two bits are the keyboard and
   * pointing device flags and are not part of the type. */
  return major == 0x05 && (minor & 0x0F) == 0x02;
}

const char *ds_hci_host_state_name(ds_hci_host_state_t st) {
  switch (st) {
    case DS_HOST_IDLE:         return "idle";
    case DS_HOST_INITIALISING: return "initialising";
    case DS_HOST_READY:        return "ready";
    case DS_HOST_INQUIRING:    return "inquiring";
    case DS_HOST_CONNECTING:   return "connecting";
    case DS_HOST_CONNECTED:    return "connected";
    case DS_HOST_FAILED:       return "failed";
    default:                    return "unknown";
  }
}

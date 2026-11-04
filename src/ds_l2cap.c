/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_l2cap.c — HID over L2CAP. See ds_l2cap.h.
 */
#include "ds_l2cap.h"

#include <string.h>

#define SIG_HDR_LEN 4   /* code, identifier, length */

/* Command Reject reasons */
#define REJ_NOT_UNDERSTOOD 0x0000
#define REJ_MTU_EXCEEDED   0x0001
#define REJ_INVALID_CID    0x0002

/* Information Response results */
#define INFO_NOT_SUPPORTED 0x0001

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void say(ds_l2cap_t *l, const char *msg) {
  if (l->cfg.log != NULL) l->cfg.log(l->cfg.ctx, msg);
}

/* ========================================================================== */
/*                              channel table                                 */
/* ========================================================================== */

static ds_l2cap_chan_t *by_psm(ds_l2cap_t *l, uint16_t psm) {
  int i;
  for (i = 0; i < DS_L2CAP_MAX_CHANNELS; i++) {
    if (l->chan[i].state != DS_L2CAP_CLOSED && l->chan[i].psm == psm) {
      return &l->chan[i];
    }
  }
  return NULL;
}

static ds_l2cap_chan_t *by_local_cid(ds_l2cap_t *l, uint16_t cid) {
  int i;
  if (cid < DS_L2CAP_CID_DYNAMIC_FIRST) return NULL;
  for (i = 0; i < DS_L2CAP_MAX_CHANNELS; i++) {
    if (l->chan[i].state != DS_L2CAP_CLOSED && l->chan[i].local_cid == cid) {
      return &l->chan[i];
    }
  }
  return NULL;
}

static ds_l2cap_chan_t *alloc_chan(ds_l2cap_t *l, uint16_t psm) {
  int i;
  if (by_psm(l, psm) != NULL) return NULL;   /* one channel per PSM */
  for (i = 0; i < DS_L2CAP_MAX_CHANNELS; i++) {
    ds_l2cap_chan_t *c = &l->chan[i];
    if (c->state != DS_L2CAP_CLOSED) continue;
    memset(c, 0, sizeof(*c));
    c->psm = psm;
    c->local_cid = l->next_cid++;
    if (l->next_cid < DS_L2CAP_CID_DYNAMIC_FIRST) {
      l->next_cid = DS_L2CAP_CID_DYNAMIC_FIRST;
    }
    return c;
  }
  return NULL;
}

static void close_chan(ds_l2cap_t *l, ds_l2cap_chan_t *c) {
  uint16_t psm;
  if (c == NULL || c->state == DS_L2CAP_CLOSED) return;
  psm = c->psm;
  memset(c, 0, sizeof(*c));
  if (l->cfg.on_close != NULL) l->cfg.on_close(l->cfg.ctx, psm);
}

static bool psm_supported(uint16_t psm) {
  /* SDP is here because a pad that pages the host opens it before settling
   * in, and a refusal costs the whole link half a minute later. Nothing
   * above this layer connects to it; it is only ever accepted. */
  return psm == DS_L2CAP_PSM_HID_CONTROL ||
         psm == DS_L2CAP_PSM_HID_INTERRUPT || psm == DS_L2CAP_PSM_SDP;
}

/* ========================================================================== */
/*                              signalling out                                */
/* ========================================================================== */

static uint8_t next_ident(ds_l2cap_t *l) {
  /* Identifier 0 is reserved and means "not a real request". */
  if (l->next_ident == 0) l->next_ident = 1;
  return l->next_ident++;
}

static bool send_sig(ds_l2cap_t *l, uint8_t code, uint8_t ident,
                     const uint8_t *data, uint16_t len) {
  if ((uint32_t)SIG_HDR_LEN + len > sizeof(l->tx)) return false;
  l->tx[0] = code;
  l->tx[1] = ident;
  put16(l->tx + 2, len);
  if (len != 0) memcpy(l->tx + SIG_HDR_LEN, data, len);
  return l->cfg.send(l->cfg.ctx, DS_L2CAP_CID_SIGNALLING, l->tx,
                     (uint16_t)(SIG_HDR_LEN + len)) == 0;
}

static bool send_conn_req(ds_l2cap_t *l, ds_l2cap_chan_t *c) {
  uint8_t p[4];
  put16(p, c->psm);
  put16(p + 2, c->local_cid);
  return send_sig(l, DS_L2CAP_CONN_REQ, next_ident(l), p, sizeof(p));
}

static bool send_conn_rsp(ds_l2cap_t *l, uint8_t ident, uint16_t dcid,
                          uint16_t scid, uint16_t result) {
  uint8_t p[8];
  put16(p, dcid);
  put16(p + 2, scid);
  put16(p + 4, result);
  put16(p + 6, 0x0000);   /* status: no further information */
  return send_sig(l, DS_L2CAP_CONN_RSP, ident, p, sizeof(p));
}

/* Our only request is an MTU, and only because leaving it out would accept
 * the 672 byte default anyway — being explicit makes the exchange readable in
 * a trace. */
static bool send_config_req(ds_l2cap_t *l, ds_l2cap_chan_t *c) {
  uint8_t p[8];
  put16(p, c->remote_cid);
  put16(p + 2, 0x0000);              /* no continuation */
  p[4] = DS_L2CAP_OPT_MTU;
  p[5] = 2;
  put16(p + 6, l->cfg.mtu);
  return send_sig(l, DS_L2CAP_CONFIG_REQ, next_ident(l), p, sizeof(p));
}

static bool send_config_rsp(ds_l2cap_t *l, uint8_t ident, uint16_t scid,
                            uint16_t flags, uint16_t result,
                            const uint8_t *opts, uint16_t opt_len) {
  uint8_t p[64];
  if ((uint32_t)6 + opt_len > sizeof(p)) opt_len = (uint16_t)(sizeof(p) - 6);
  put16(p, scid);
  put16(p + 2, flags);
  put16(p + 4, result);
  if (opt_len != 0) memcpy(p + 6, opts, opt_len);
  return send_sig(l, DS_L2CAP_CONFIG_RSP, ident, p, (uint16_t)(6 + opt_len));
}

static bool send_cmd_reject(ds_l2cap_t *l, uint8_t ident, uint16_t reason,
                            const uint8_t *data, uint16_t len) {
  uint8_t p[6];
  put16(p, reason);
  if (len > 4) len = 4;
  if (len != 0) memcpy(p + 2, data, len);
  return send_sig(l, DS_L2CAP_CMD_REJECT, ident, p, (uint16_t)(2 + len));
}

/* ========================================================================== */
/*                            configuration options                           */
/* ========================================================================== */

/* Walks a configuration option list, taking what we understand and gathering
 * what we do not. An option with the hint bit set may be ignored silently;
 * anything else we do not recognise has to come back as a rejection, or the
 * peer will sit waiting for a capability we never had. */
static uint16_t scan_options(ds_l2cap_chan_t *c, const uint8_t *opts,
                             uint16_t len, uint8_t *unknown,
                             uint16_t unknown_cap) {
  uint16_t off = 0, unknown_len = 0;

  while (off + 2 <= len) {
    uint8_t type = opts[off];
    uint8_t olen = opts[off + 1];
    const uint8_t *val = opts + off + 2;

    if ((uint32_t)off + 2 + olen > len) break;   /* truncated, stop */

    if ((type & 0x7F) == DS_L2CAP_OPT_MTU && olen == 2) {
      c->remote_mtu = get16(val);
    } else if ((type & DS_L2CAP_OPT_HINT) != 0) {
      /* a hint; ignoring it is allowed */
    } else if ((type & 0x7F) != DS_L2CAP_OPT_FLUSH_TIMEOUT) {
      /* Flush timeout is accepted as-is; anything else is refused. */
      if ((uint32_t)unknown_len + 2 + olen <= unknown_cap) {
        memcpy(unknown + unknown_len, opts + off, (size_t)(2 + olen));
        unknown_len = (uint16_t)(unknown_len + 2 + olen);
      }
    }
    off = (uint16_t)(off + 2 + olen);
  }
  return unknown_len;
}

/* Pulls an MTU out of a rejection so we can try again with what they asked
 * for. Returns 0 when they did not suggest one. */
static uint16_t suggested_mtu(const uint8_t *opts, uint16_t len) {
  uint16_t off = 0;
  while (off + 2 <= len) {
    uint8_t type = opts[off];
    uint8_t olen = opts[off + 1];
    if ((uint32_t)off + 2 + olen > len) break;
    if ((type & 0x7F) == DS_L2CAP_OPT_MTU && olen == 2) {
      return get16(opts + off + 2);
    }
    off = (uint16_t)(off + 2 + olen);
  }
  return 0;
}

/* ========================================================================== */
/*                            signalling handlers                             */
/* ========================================================================== */

static void check_open(ds_l2cap_t *l, ds_l2cap_chan_t *c) {
  uint16_t psm;
  if (c->state != DS_L2CAP_CONFIGURING) return;
  if (!c->config_sent_ok || !c->config_recv_ok) return;

  c->state = DS_L2CAP_OPEN;
  psm = c->psm;
  say(l, "l2cap: channel open");
  if (l->cfg.on_open != NULL) l->cfg.on_open(l->cfg.ctx, psm);

  /* Interrupt goes up only after control, because some devices refuse it the
   * other way round. Note the callback above may have closed things, so
   * re-check rather than trusting the pointer. */
  if (l->auto_hid && psm == DS_L2CAP_PSM_HID_CONTROL &&
      by_psm(l, DS_L2CAP_PSM_HID_INTERRUPT) == NULL) {
    ds_l2cap_connect(l, DS_L2CAP_PSM_HID_INTERRUPT);
  }
}

static void on_conn_req(ds_l2cap_t *l, uint8_t ident, const uint8_t *d,
                        uint16_t len) {
  uint16_t psm, scid;
  ds_l2cap_chan_t *c;

  if (len < 4) return;
  psm = get16(d);
  scid = get16(d + 2);

  if (!psm_supported(psm)) {
    send_conn_rsp(l, ident, 0, scid, DS_L2CAP_CONN_NO_PSM);
    return;
  }

  /* Both ends can decide to open the same channel at the same moment — a
   * bonded device that calls in usually opens its own HID channels while the
   * host is opening them too. Refusing theirs because ours is half-built
   * makes the device give up and drop the link, which looks exactly like a
   * connection that appears and then vanishes. Give way: an attempt of ours
   * that has not opened yet is worth less than a request already in hand. */
  c = by_psm(l, psm);
  if (c != NULL && c->state != DS_L2CAP_OPEN) {
    say(l, "l2cap: both ends opened at once, taking theirs");
    close_chan(l, c);
  }

  c = alloc_chan(l, psm);
  if (c == NULL) {
    send_conn_rsp(l, ident, 0, scid, DS_L2CAP_CONN_NO_RESOURCES);
    return;
  }
  c->remote_cid = scid;
  c->state = DS_L2CAP_CONFIGURING;
  say(l, "l2cap: incoming channel accepted");
  send_conn_rsp(l, ident, c->local_cid, scid, DS_L2CAP_CONN_SUCCESS);
  send_config_req(l, c);
}

static void on_conn_rsp(ds_l2cap_t *l, const uint8_t *d, uint16_t len) {
  uint16_t dcid, scid, result;
  ds_l2cap_chan_t *c;

  if (len < 8) return;
  dcid = get16(d);
  scid = get16(d + 2);
  result = get16(d + 4);

  /* The source cid is ours: it is echoed from the request we sent. */
  c = by_local_cid(l, scid);
  if (c == NULL || c->state != DS_L2CAP_CONNECTING) return;

  if (result == DS_L2CAP_CONN_PENDING) return;   /* they will follow up */
  if (result != DS_L2CAP_CONN_SUCCESS) {
    say(l, "l2cap: channel refused");
    close_chan(l, c);
    return;
  }
  c->remote_cid = dcid;
  c->state = DS_L2CAP_CONFIGURING;
  send_config_req(l, c);
}

static void on_config_req(ds_l2cap_t *l, uint8_t ident, const uint8_t *d,
                          uint16_t len) {
  uint8_t unknown[32];
  uint16_t dcid, flags, unknown_len;
  ds_l2cap_chan_t *c;

  if (len < 4) return;
  dcid = get16(d);
  flags = get16(d + 2);

  c = by_local_cid(l, dcid);
  if (c == NULL) {
    send_cmd_reject(l, ident, REJ_INVALID_CID, d, 4);
    return;
  }
  unknown_len = scan_options(c, d + 4, (uint16_t)(len - 4), unknown,
                             sizeof(unknown));
  if (unknown_len != 0) {
    say(l, "l2cap: rejecting unknown config option");
    send_config_rsp(l, ident, c->remote_cid, 0, DS_L2CAP_CFG_UNKNOWN_OPT,
                    unknown, unknown_len);
    return;
  }
  /* The continuation bit means more options are coming in another request;
   * agreement is not complete until one arrives with the bit clear. */
  send_config_rsp(l, ident, c->remote_cid, flags & 0x0001,
                  DS_L2CAP_CFG_SUCCESS, NULL, 0);
  if ((flags & 0x0001) == 0) {
    c->config_recv_ok = true;
    check_open(l, c);
  }
}

static void on_config_rsp(ds_l2cap_t *l, const uint8_t *d, uint16_t len) {
  uint16_t scid, flags, result;
  ds_l2cap_chan_t *c;

  if (len < 6) return;
  scid = get16(d);
  flags = get16(d + 2);
  result = get16(d + 4);

  c = by_local_cid(l, scid);
  if (c == NULL || c->state != DS_L2CAP_CONFIGURING) return;
  if ((flags & 0x0001) != 0) return;   /* more of the answer to come */

  if (result == DS_L2CAP_CFG_SUCCESS) {
    c->config_sent_ok = true;
    check_open(l, c);
    return;
  }
  /* The one renegotiation worth doing: they want a smaller MTU than we asked
   * for. Take theirs and ask once more. Anything else, or a second refusal,
   * and the channel is not going to happen. */
  if (result == DS_L2CAP_CFG_UNACCEPTABLE && !c->config_retried) {
    uint16_t mtu = suggested_mtu(d + 6, (uint16_t)(len - 6));
    if (mtu != 0) {
      uint8_t p[8];
      c->config_retried = true;
      c->remote_mtu = mtu;
      say(l, "l2cap: retrying config with their mtu");
      /* Ask for what they offered rather than what we wanted. */
      put16(p, c->remote_cid);
      put16(p + 2, 0x0000);
      p[4] = DS_L2CAP_OPT_MTU;
      p[5] = 2;
      put16(p + 6, mtu);
      send_sig(l, DS_L2CAP_CONFIG_REQ, next_ident(l), p, sizeof(p));
      return;
    }
  }
  say(l, "l2cap: config refused");
  close_chan(l, c);
}

static void on_disconn_req(ds_l2cap_t *l, uint8_t ident, const uint8_t *d,
                           uint16_t len) {
  uint16_t dcid, scid;
  ds_l2cap_chan_t *c;

  if (len < 4) return;
  dcid = get16(d);
  scid = get16(d + 2);

  c = by_local_cid(l, dcid);
  /* The response echoes the request whether or not we knew the channel. */
  send_sig(l, DS_L2CAP_DISCONN_RSP, ident, d, 4);
  if (c != NULL && c->remote_cid == scid) {
    say(l, "l2cap: peer closed a channel");
    close_chan(l, c);
  }
}

static void on_disconn_rsp(ds_l2cap_t *l, const uint8_t *d, uint16_t len) {
  if (len < 4) return;
  close_chan(l, by_local_cid(l, get16(d + 2)));
}

static void on_info_req(ds_l2cap_t *l, uint8_t ident, const uint8_t *d,
                        uint16_t len) {
  uint8_t p[4];
  if (len < 2) return;
  /* We support none of the extended features anyone asks about, and saying so
   * is faster than being asked twice. */
  put16(p, get16(d));
  put16(p + 2, INFO_NOT_SUPPORTED);
  send_sig(l, DS_L2CAP_INFO_RSP, ident, p, sizeof(p));
}

static void on_cmd_reject(ds_l2cap_t *l, const uint8_t *d, uint16_t len) {
  say(l, "l2cap: command rejected");
  if (len >= 6 && get16(d) == REJ_INVALID_CID) {
    /* reason, then the source and destination cid they could not place. */
    close_chan(l, by_local_cid(l, get16(d + 2)));
  }
}

static void dispatch_sig(ds_l2cap_t *l, uint8_t code, uint8_t ident,
                         const uint8_t *d, uint16_t len) {
  switch (code) {
    case DS_L2CAP_CONN_REQ:     on_conn_req(l, ident, d, len); break;
    case DS_L2CAP_CONN_RSP:     on_conn_rsp(l, d, len); break;
    case DS_L2CAP_CONFIG_REQ:   on_config_req(l, ident, d, len); break;
    case DS_L2CAP_CONFIG_RSP:   on_config_rsp(l, d, len); break;
    case DS_L2CAP_DISCONN_REQ:  on_disconn_req(l, ident, d, len); break;
    case DS_L2CAP_DISCONN_RSP:  on_disconn_rsp(l, d, len); break;
    case DS_L2CAP_ECHO_REQ:     send_sig(l, DS_L2CAP_ECHO_RSP, ident, d, len); break;
    case DS_L2CAP_INFO_REQ:     on_info_req(l, ident, d, len); break;
    case DS_L2CAP_CMD_REJECT:   on_cmd_reject(l, d, len); break;
    case DS_L2CAP_ECHO_RSP:
    case DS_L2CAP_INFO_RSP:
      break;   /* nothing here ever asks */
    default:
      send_cmd_reject(l, ident, REJ_NOT_UNDERSTOOD, NULL, 0);
      break;
  }
}

/* ========================================================================== */
/*                                public API                                  */
/* ========================================================================== */

bool ds_l2cap_init(ds_l2cap_t *l, const ds_l2cap_config_t *cfg) {
  if (l == NULL || cfg == NULL || cfg->send == NULL) return false;

  memset(l, 0, sizeof(*l));
  l->cfg = *cfg;
  if (l->cfg.mtu == 0) l->cfg.mtu = DS_L2CAP_DEFAULT_MTU;
  l->next_ident = 1;
  l->next_cid = DS_L2CAP_CID_DYNAMIC_FIRST;
  return true;
}

void ds_l2cap_reset(ds_l2cap_t *l) {
  int i;
  if (l == NULL) return;
  /* The ACL link is gone, so no disconnection requests: there is nothing to
   * send them over. The callbacks still fire so the app can tidy up. */
  for (i = 0; i < DS_L2CAP_MAX_CHANNELS; i++) close_chan(l, &l->chan[i]);
  l->auto_hid = false;
  l->next_cid = DS_L2CAP_CID_DYNAMIC_FIRST;
}

bool ds_l2cap_connect(ds_l2cap_t *l, uint16_t psm) {
  ds_l2cap_chan_t *c;
  if (l == NULL) return false;

  c = alloc_chan(l, psm);
  if (c == NULL) return false;
  c->state = DS_L2CAP_CONNECTING;
  if (!send_conn_req(l, c)) {
    close_chan(l, c);
    return false;
  }
  return true;
}

bool ds_l2cap_connect_hid(ds_l2cap_t *l) {
  if (l == NULL) return false;
  l->auto_hid = true;
  if (ds_l2cap_connect(l, DS_L2CAP_PSM_HID_CONTROL)) return true;
  l->auto_hid = false;
  return false;
}

bool ds_l2cap_disconnect(ds_l2cap_t *l, uint16_t psm) {
  uint8_t p[4];
  ds_l2cap_chan_t *c = (l != NULL) ? by_psm(l, psm) : NULL;
  if (c == NULL) return false;

  put16(p, c->remote_cid);
  put16(p + 2, c->local_cid);
  return send_sig(l, DS_L2CAP_DISCONN_REQ, next_ident(l), p, sizeof(p));
}

void ds_l2cap_handle(ds_l2cap_t *l, uint16_t cid, const uint8_t *data,
                      uint16_t len) {
  if (l == NULL || data == NULL) return;

  if (cid == DS_L2CAP_CID_SIGNALLING) {
    /* One packet may carry several commands back to back. */
    uint16_t off = 0;
    while ((uint32_t)off + SIG_HDR_LEN <= len) {
      uint16_t clen = get16(data + off + 2);
      if ((uint32_t)off + SIG_HDR_LEN + clen > len) break;
      dispatch_sig(l, data[off], data[off + 1], data + off + SIG_HDR_LEN, clen);
      off = (uint16_t)(off + SIG_HDR_LEN + clen);
    }
    return;
  }

  {
    ds_l2cap_chan_t *c = by_local_cid(l, cid);
    /* Both of these used to be a bare return. A packet nobody can place, or
     * one that arrived before its channel finished opening, is exactly the
     * kind of thing that gets waited on at the other end. */
    if (c == NULL) {
      say(l, "l2cap: packet on an unknown channel");
      return;
    }
    if (c->state != DS_L2CAP_OPEN) {
      say(l, "l2cap: packet on a channel still opening");
      return;
    }
    if (l->cfg.on_data != NULL) {
      l->cfg.on_data(l->cfg.ctx, c->psm, data, len);
    }
  }
}

bool ds_l2cap_send(ds_l2cap_t *l, uint16_t psm, const uint8_t *data,
                    uint16_t len) {
  ds_l2cap_chan_t *c = (l != NULL) ? by_psm(l, psm) : NULL;
  if (c == NULL || c->state != DS_L2CAP_OPEN) return false;
  if (len != 0 && data == NULL) return false;
  /* Sending past the peer's MTU is not something it has to notice; it is
   * allowed to drop the packet or the channel. Refuse instead. */
  if (c->remote_mtu != 0 && len > c->remote_mtu) return false;

  return l->cfg.send(l->cfg.ctx, c->remote_cid, data, len) == 0;
}

bool ds_l2cap_is_open(const ds_l2cap_t *l, uint16_t psm) {
  const ds_l2cap_chan_t *c = ds_l2cap_channel(l, psm);
  return c != NULL && c->state == DS_L2CAP_OPEN;
}

bool ds_l2cap_hid_ready(const ds_l2cap_t *l) {
  return ds_l2cap_is_open(l, DS_L2CAP_PSM_HID_CONTROL) &&
         ds_l2cap_is_open(l, DS_L2CAP_PSM_HID_INTERRUPT);
}

const ds_l2cap_chan_t *ds_l2cap_channel(const ds_l2cap_t *l, uint16_t psm) {
  int i;
  if (l == NULL) return NULL;
  for (i = 0; i < DS_L2CAP_MAX_CHANNELS; i++) {
    if (l->chan[i].state != DS_L2CAP_CLOSED && l->chan[i].psm == psm) {
      return &l->chan[i];
    }
  }
  return NULL;
}

const char *ds_l2cap_state_name(ds_l2cap_chan_state_t st) {
  switch (st) {
    case DS_L2CAP_CLOSED:      return "closed";
    case DS_L2CAP_CONNECTING:  return "connecting";
    case DS_L2CAP_CONFIGURING: return "configuring";
    case DS_L2CAP_OPEN:        return "open";
    default:                    return "unknown";
  }
}

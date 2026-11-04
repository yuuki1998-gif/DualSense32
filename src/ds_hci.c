/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_hci.c — HCI packet layer. See ds_hci.h for what this is and is not.
 *
 * Zero dependencies beyond string.h, so it builds and can be tested anywhere,
 * on any target, with or without an RTOS. Nothing here allocates, blocks or
 * keeps state except the ACL reassembler the caller owns.
 */
#include "ds_hci.h"

#include <string.h>

/* ========================================================================== */
/*                          little endian helpers                             */
/* ========================================================================== */

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

static void put24(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
}

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get24(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* ========================================================================== */
/*                               C O M M A N D S                              */
/* ========================================================================== */

uint16_t ds_hci_cmd(uint8_t *buf, uint16_t buflen, uint16_t opcode,
                     const uint8_t *params, uint8_t plen) {
  if (buf == NULL) return 0;
  if (plen != 0 && params == NULL) return 0;
  if (buflen < (uint16_t)(DS_HCI_CMD_HDR_LEN + plen)) return 0;

  put16(buf, opcode);
  buf[2] = plen;
  if (plen != 0) memcpy(buf + DS_HCI_CMD_HDR_LEN, params, plen);
  return (uint16_t)(DS_HCI_CMD_HDR_LEN + plen);
}

uint16_t ds_hci_cmd_reset(uint8_t *buf, uint16_t buflen) {
  return ds_hci_cmd(buf, buflen, DS_HCI_RESET, NULL, 0);
}

uint16_t ds_hci_cmd_set_event_mask(uint8_t *buf, uint16_t buflen,
                                    uint64_t mask) {
  uint8_t p[8];
  int i;
  for (i = 0; i < 8; i++) p[i] = (uint8_t)((mask >> (8 * i)) & 0xFF);
  return ds_hci_cmd(buf, buflen, DS_HCI_SET_EVENT_MASK, p, sizeof(p));
}

uint16_t ds_hci_cmd_write_scan_enable(uint8_t *buf, uint16_t buflen,
                                       uint8_t scan) {
  if (scan > DS_HCI_SCAN_BOTH) return 0;
  return ds_hci_cmd(buf, buflen, DS_HCI_WRITE_SCAN_ENABLE, &scan, 1);
}

uint16_t ds_hci_cmd_write_local_name(uint8_t *buf, uint16_t buflen,
                                      const char *name) {
  /* The parameter is a fixed 248 bytes whatever the name length, zero padded.
   * Sending the short form is a common mistake and the controller rejects it
   * with Invalid HCI Command Parameters. */
  uint8_t p[DS_HCI_LOCAL_NAME_LEN];
  size_t n = 0;

  if (name != NULL) {
    while (n < DS_HCI_LOCAL_NAME_LEN && name[n] != '\0') n++;
    /* Leave room for the terminator unless the name fills the field exactly. */
    if (n == DS_HCI_LOCAL_NAME_LEN) n = DS_HCI_LOCAL_NAME_LEN - 1;
  }
  memset(p, 0, sizeof(p));
  if (n != 0) memcpy(p, name, n);
  return ds_hci_cmd(buf, buflen, DS_HCI_WRITE_LOCAL_NAME, p, sizeof(p));
}

uint16_t ds_hci_cmd_write_class_of_device(uint8_t *buf, uint16_t buflen,
                                           uint32_t cod) {
  uint8_t p[3];
  put24(p, cod);
  return ds_hci_cmd(buf, buflen, DS_HCI_WRITE_CLASS_OF_DEVICE, p, sizeof(p));
}

uint16_t ds_hci_cmd_write_simple_pairing_mode(uint8_t *buf, uint16_t buflen,
                                               bool on) {
  uint8_t p = on ? 0x01 : 0x00;
  return ds_hci_cmd(buf, buflen, DS_HCI_WRITE_SIMPLE_PAIRING_MODE, &p, 1);
}

uint16_t ds_hci_cmd_inquiry(uint8_t *buf, uint16_t buflen, uint32_t lap,
                             uint8_t length, uint8_t max_responses) {
  uint8_t p[5];
  if (length < 1 || length > 48) return 0;
  put24(p, lap);
  p[3] = length;
  p[4] = max_responses;
  return ds_hci_cmd(buf, buflen, DS_HCI_INQUIRY, p, sizeof(p));
}

uint16_t ds_hci_cmd_create_connection(uint8_t *buf, uint16_t buflen,
                                       const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                       uint16_t packet_types,
                                       uint8_t page_scan_repetition_mode,
                                       uint16_t clock_offset,
                                       bool allow_role_switch) {
  uint8_t p[13];
  if (addr == NULL) return 0;
  memcpy(p, addr, DS_HCI_BD_ADDR_LEN);
  put16(p + 6, packet_types);
  p[8] = page_scan_repetition_mode;
  p[9] = 0x00;                       /* reserved, must be zero */
  put16(p + 10, clock_offset);
  p[12] = allow_role_switch ? 0x01 : 0x00;
  return ds_hci_cmd(buf, buflen, DS_HCI_CREATE_CONNECTION, p, sizeof(p));
}

uint16_t ds_hci_cmd_handle(uint8_t *buf, uint16_t buflen, uint16_t opcode,
                            uint16_t handle) {
  uint8_t p[2];
  p[0] = (uint8_t)(handle & 0xFF);
  p[1] = (uint8_t)(handle >> 8);
  return ds_hci_cmd(buf, buflen, opcode, p, sizeof(p));
}

uint16_t ds_hci_cmd_read_remote_ext_features(uint8_t *buf, uint16_t buflen,
                                              uint16_t handle, uint8_t page) {
  uint8_t p[3];
  p[0] = (uint8_t)(handle & 0xFF);
  p[1] = (uint8_t)(handle >> 8);
  p[2] = page;
  return ds_hci_cmd(buf, buflen, DS_HCI_READ_REMOTE_EXT_FEATURES, p,
                     sizeof(p));
}

uint16_t ds_hci_cmd_accept_connection(uint8_t *buf, uint16_t buflen,
                                       const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                       uint8_t role) {
  uint8_t p[7];
  if (addr == NULL) return 0;
  memcpy(p, addr, DS_HCI_BD_ADDR_LEN);
  p[6] = role;
  return ds_hci_cmd(buf, buflen, DS_HCI_ACCEPT_CONNECTION_REQUEST, p,
                     sizeof(p));
}

uint16_t ds_hci_cmd_disconnect(uint8_t *buf, uint16_t buflen, uint16_t handle,
                                uint8_t reason) {
  uint8_t p[3];
  put16(p, handle);
  p[2] = reason;
  return ds_hci_cmd(buf, buflen, DS_HCI_DISCONNECT, p, sizeof(p));
}

uint16_t ds_hci_cmd_link_key_reply(uint8_t *buf, uint16_t buflen,
                                    const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                    const uint8_t key[DS_HCI_LINK_KEY_LEN]) {
  uint8_t p[DS_HCI_BD_ADDR_LEN + DS_HCI_LINK_KEY_LEN];
  if (addr == NULL || key == NULL) return 0;
  memcpy(p, addr, DS_HCI_BD_ADDR_LEN);
  memcpy(p + DS_HCI_BD_ADDR_LEN, key, DS_HCI_LINK_KEY_LEN);
  return ds_hci_cmd(buf, buflen, DS_HCI_LINK_KEY_REQUEST_REPLY, p, sizeof(p));
}

uint16_t ds_hci_cmd_link_key_neg_reply(uint8_t *buf, uint16_t buflen,
                                        const uint8_t addr[DS_HCI_BD_ADDR_LEN]) {
  if (addr == NULL) return 0;
  return ds_hci_cmd(buf, buflen, DS_HCI_LINK_KEY_REQUEST_NEG_REPLY, addr,
                     DS_HCI_BD_ADDR_LEN);
}

uint16_t ds_hci_cmd_io_capability_reply(uint8_t *buf, uint16_t buflen,
                                         const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                         uint8_t io_capability,
                                         uint8_t oob_present,
                                         uint8_t auth_requirements) {
  uint8_t p[9];
  if (addr == NULL) return 0;
  if (io_capability > DS_HCI_IO_NO_INPUT_OUTPUT) return 0;
  if (auth_requirements > DS_HCI_AUTH_MITM_GENERAL) return 0;
  memcpy(p, addr, DS_HCI_BD_ADDR_LEN);
  p[6] = io_capability;
  p[7] = oob_present;
  p[8] = auth_requirements;
  return ds_hci_cmd(buf, buflen, DS_HCI_IO_CAPABILITY_REQUEST_REPLY, p,
                     sizeof(p));
}

uint16_t ds_hci_cmd_user_confirmation_reply(uint8_t *buf, uint16_t buflen,
                                             const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                             bool accept) {
  if (addr == NULL) return 0;
  return ds_hci_cmd(buf, buflen,
                     accept ? DS_HCI_USER_CONFIRMATION_REPLY
                            : DS_HCI_USER_CONFIRMATION_NEG_REPLY,
                     addr, DS_HCI_BD_ADDR_LEN);
}

uint16_t ds_hci_cmd_authentication_requested(uint8_t *buf, uint16_t buflen,
                                              uint16_t handle) {
  uint8_t p[2];
  put16(p, handle);
  return ds_hci_cmd(buf, buflen, DS_HCI_AUTHENTICATION_REQUESTED, p,
                     sizeof(p));
}

uint16_t ds_hci_cmd_set_connection_encryption(uint8_t *buf, uint16_t buflen,
                                               uint16_t handle, bool enable) {
  uint8_t p[3];
  put16(p, handle);
  p[2] = enable ? 0x01 : 0x00;
  return ds_hci_cmd(buf, buflen, DS_HCI_SET_CONNECTION_ENCRYPTION, p,
                     sizeof(p));
}

/* ========================================================================== */
/*                                 E V E N T S                                */
/* ========================================================================== */

bool ds_hci_parse_event(const uint8_t *pkt, uint16_t len,
                         ds_hci_event_t *ev) {
  if (pkt == NULL || ev == NULL) return false;
  if (len < DS_HCI_EVT_HDR_LEN) return false;
  /* A short read is a transport bug, not a truncated event. Refuse it rather
   * than hand out a params pointer that runs off the end of the buffer. */
  if ((uint16_t)(DS_HCI_EVT_HDR_LEN + pkt[1]) > len) return false;

  ev->code = pkt[0];
  ev->plen = pkt[1];
  ev->params = pkt + DS_HCI_EVT_HDR_LEN;
  return true;
}

/* Every parser below shares this guard: right event, long enough. */
static bool evt_ok(const ds_hci_event_t *ev, uint8_t code, uint8_t min_len) {
  return ev != NULL && ev->params != NULL && ev->code == code &&
         ev->plen >= min_len;
}

bool ds_hci_parse_cmd_complete(const ds_hci_event_t *ev,
                                ds_hci_cmd_complete_t *out) {
  if (out == NULL) return false;
  if (!evt_ok(ev, DS_HCI_EVT_COMMAND_COMPLETE, 3)) return false;

  out->num_cmd_packets = ev->params[0];
  out->opcode = get16(ev->params + 1);
  out->ret = ev->params + 3;
  out->ret_len = (uint8_t)(ev->plen - 3);
  /* Almost every command returns a status first, but a few (Reset aside, the
   * NOP opcode 0x0000 used for flow control) return nothing at all. */
  out->status = (out->ret_len >= 1) ? out->ret[0] : 0xFF;
  return true;
}

bool ds_hci_parse_cmd_status(const ds_hci_event_t *ev,
                              ds_hci_cmd_status_t *out) {
  if (out == NULL) return false;
  if (!evt_ok(ev, DS_HCI_EVT_COMMAND_STATUS, 4)) return false;

  out->status = ev->params[0];
  out->num_cmd_packets = ev->params[1];
  out->opcode = get16(ev->params + 2);
  return true;
}

bool ds_hci_parse_conn_complete(const ds_hci_event_t *ev,
                                 ds_hci_conn_complete_t *out) {
  if (out == NULL) return false;
  if (!evt_ok(ev, DS_HCI_EVT_CONNECTION_COMPLETE, 11)) return false;

  out->status = ev->params[0];
  out->handle = (uint16_t)(get16(ev->params + 1) & 0x0FFF);
  memcpy(out->addr, ev->params + 3, DS_HCI_BD_ADDR_LEN);
  out->link_type = ev->params[9];
  out->encryption = ev->params[10];
  return true;
}

bool ds_hci_parse_conn_request(const ds_hci_event_t *ev,
                                ds_hci_conn_request_t *out) {
  if (out == NULL) return false;
  if (!evt_ok(ev, DS_HCI_EVT_CONNECTION_REQUEST, 10)) return false;

  memcpy(out->addr, ev->params, DS_HCI_BD_ADDR_LEN);
  out->class_of_device = get24(ev->params + 6);
  out->link_type = ev->params[9];
  return true;
}

bool ds_hci_parse_disconn_complete(const ds_hci_event_t *ev,
                                    ds_hci_disconn_complete_t *out) {
  if (out == NULL) return false;
  if (!evt_ok(ev, DS_HCI_EVT_DISCONNECTION_COMPLETE, 4)) return false;

  out->status = ev->params[0];
  out->handle = (uint16_t)(get16(ev->params + 1) & 0x0FFF);
  out->reason = ev->params[3];
  return true;
}

/* --- inquiry results -----------------------------------------------------
 *
 * The three inquiry events look alike and are not. Only the first seven bytes
 * of an entry are common; adding RSSI dropped one of the two reserved bytes,
 * so the class of device and clock offset sit one byte earlier in 0x22 and
 * 0x2F than they do in the original 0x02. Getting this wrong yields a class
 * of device that is almost right, which is worse than obviously wrong. */
typedef struct {
  uint8_t stride;
  uint8_t cod_off;
  uint8_t clock_off;
  int8_t rssi_off;    /* negative when the event has no RSSI */
} inquiry_layout_t;

static bool inquiry_layout(uint8_t code, inquiry_layout_t *l) {
  switch (code) {
    case DS_HCI_EVT_INQUIRY_RESULT:
      /* addr 0-5, psrm 6, reserved 7, reserved 8, cod 9-11, clock 12-13 */
      l->stride = 14; l->cod_off = 9; l->clock_off = 12; l->rssi_off = -1;
      return true;
    case DS_HCI_EVT_INQUIRY_RESULT_RSSI:
      /* addr 0-5, psrm 6, reserved 7, cod 8-10, clock 11-12, rssi 13 */
      l->stride = 14; l->cod_off = 8; l->clock_off = 11; l->rssi_off = 13;
      return true;
    case DS_HCI_EVT_EXTENDED_INQUIRY_RESULT:
      /* Same fixed part as with-RSSI, then 240 bytes of EIR. The spec fixes
       * the response count at one, so the stride is never exercised. */
      l->stride = 254; l->cod_off = 8; l->clock_off = 11; l->rssi_off = 13;
      return true;
    default:
      return false;
  }
}

uint8_t ds_hci_inquiry_result_count(const ds_hci_event_t *ev) {
  inquiry_layout_t l;
  uint8_t claimed;

  if (ev == NULL || ev->params == NULL || ev->plen < 1) return 0;
  if (!inquiry_layout(ev->code, &l)) return 0;

  claimed = ev->params[0];
  /* Trust the length, not the count: a controller claiming more responses
   * than it sent would walk us off the end. */
  if ((uint32_t)1 + (uint32_t)claimed * l.stride > ev->plen) {
    return (uint8_t)((ev->plen - 1) / l.stride);
  }
  return claimed;
}

bool ds_hci_parse_inquiry_result(const ds_hci_event_t *ev, uint8_t index,
                                  ds_hci_inquiry_result_t *out) {
  inquiry_layout_t l;
  const uint8_t *e;

  if (out == NULL) return false;
  if (index >= ds_hci_inquiry_result_count(ev)) return false;
  if (!inquiry_layout(ev->code, &l)) return false;

  e = ev->params + 1 + (size_t)index * l.stride;
  memcpy(out->addr, e, DS_HCI_BD_ADDR_LEN);
  out->page_scan_repetition_mode = e[6];
  out->class_of_device = get24(e + l.cod_off);
  out->clock_offset = get16(e + l.clock_off);
  if (l.rssi_off >= 0) {
    out->rssi = (int8_t)e[l.rssi_off];
    out->has_rssi = true;
  } else {
    out->rssi = 0;
    out->has_rssi = false;
  }
  return true;
}

bool ds_hci_parse_addr_event(const ds_hci_event_t *ev,
                              uint8_t addr[DS_HCI_BD_ADDR_LEN]) {
  if (ev == NULL || addr == NULL || ev->params == NULL) return false;
  if (ev->code != DS_HCI_EVT_LINK_KEY_REQUEST &&
      ev->code != DS_HCI_EVT_IO_CAPABILITY_REQUEST) {
    return false;
  }
  if (ev->plen < DS_HCI_BD_ADDR_LEN) return false;

  memcpy(addr, ev->params, DS_HCI_BD_ADDR_LEN);
  return true;
}

bool ds_hci_parse_link_key_notification(const ds_hci_event_t *ev,
                                         ds_hci_link_key_notification_t *out) {
  if (out == NULL) return false;
  if (!evt_ok(ev, DS_HCI_EVT_LINK_KEY_NOTIFICATION, 23)) return false;

  memcpy(out->addr, ev->params, DS_HCI_BD_ADDR_LEN);
  memcpy(out->key, ev->params + 6, DS_HCI_LINK_KEY_LEN);
  out->key_type = ev->params[22];
  return true;
}

bool ds_hci_parse_user_confirmation(const ds_hci_event_t *ev,
                                     ds_hci_user_confirmation_t *out) {
  if (out == NULL) return false;
  if (!evt_ok(ev, DS_HCI_EVT_USER_CONFIRMATION_REQUEST, 10)) return false;

  memcpy(out->addr, ev->params, DS_HCI_BD_ADDR_LEN);
  out->numeric_value = get32(ev->params + 6);
  return true;
}

/* ========================================================================== */
/*                               A C L   D A T A                              */
/* ========================================================================== */

bool ds_hci_parse_acl(const uint8_t *pkt, uint16_t pktlen,
                       ds_hci_acl_t *out) {
  uint16_t hf, dlen;

  if (pkt == NULL || out == NULL) return false;
  if (pktlen < DS_HCI_ACL_HDR_LEN) return false;

  hf = get16(pkt);
  dlen = get16(pkt + 2);
  if ((uint32_t)DS_HCI_ACL_HDR_LEN + dlen > pktlen) return false;

  out->handle = (uint16_t)(hf & 0x0FFF);
  out->pb = (uint8_t)((hf >> 12) & 0x03);
  out->bc = (uint8_t)((hf >> 14) & 0x03);
  out->data = pkt + DS_HCI_ACL_HDR_LEN;
  out->len = dlen;
  return true;
}

uint16_t ds_hci_acl_header(uint8_t *buf, uint16_t buflen, uint16_t handle,
                            uint8_t pb, uint16_t payload_len) {
  if (buf == NULL || buflen < DS_HCI_ACL_HDR_LEN) return 0;
  if (handle > 0x0FFF || pb > DS_HCI_PB_COMPLETE) return 0;

  put16(buf, (uint16_t)((handle & 0x0FFF) | ((uint16_t)pb << 12)));
  put16(buf + 2, payload_len);
  return DS_HCI_ACL_HDR_LEN;
}

void ds_hci_reasm_reset(ds_hci_reasm_t *r) {
  if (r == NULL) return;
  r->len = 0;
  r->want = 0;
  r->handle = 0;
}

ds_hci_reasm_result_t ds_hci_reasm_push(ds_hci_reasm_t *r,
                                          const ds_hci_acl_t *acl,
                                          const uint8_t **out,
                                          uint16_t *out_len) {
  if (r == NULL || acl == NULL || acl->data == NULL) return DS_HCI_REASM_ERROR;

  if (acl->pb == DS_HCI_PB_CONTINUING) {
    /* A continuation with nothing to continue means we joined mid-packet or
     * dropped a fragment. Either way the stream is unusable until the next
     * first fragment, so say so instead of assembling garbage. */
    if (r->want == 0) return DS_HCI_REASM_ERROR;
    if (acl->handle != r->handle) return DS_HCI_REASM_ERROR;
    if ((uint32_t)r->len + acl->len > r->want) {
      ds_hci_reasm_reset(r);
      return DS_HCI_REASM_ERROR;
    }
    memcpy(r->buf + r->len, acl->data, acl->len);
    r->len = (uint16_t)(r->len + acl->len);
  } else {
    /* A first fragment while a packet is still open means the previous one
     * was lost. Start over rather than splice two halves together. */
    uint16_t l2cap_len;

    ds_hci_reasm_reset(r);
    if (acl->len < 2) return DS_HCI_REASM_ERROR;

    l2cap_len = get16(acl->data);
    /* L2CAP length counts the payload only; the 2 byte length and 2 byte
     * channel id in front of it are not included. */
    if ((uint32_t)l2cap_len + 4 > DS_HCI_REASM_MAX) return DS_HCI_REASM_ERROR;
    r->want = (uint16_t)(l2cap_len + 4);
    if (acl->len > r->want) {
      ds_hci_reasm_reset(r);
      return DS_HCI_REASM_ERROR;
    }
    memcpy(r->buf, acl->data, acl->len);
    r->len = acl->len;
    r->handle = acl->handle;
  }

  if (r->len < r->want) return DS_HCI_REASM_NEED_MORE;

  if (out != NULL) *out = r->buf;
  if (out_len != NULL) *out_len = r->len;
  r->want = 0;   /* keep the bytes; the caller reads them before the next push */
  return DS_HCI_REASM_DONE;
}

/* ========================================================================== */
/*                               H E L P E R S                                */
/* ========================================================================== */

static const char HEX[] = "0123456789ABCDEF";

void ds_hci_addr_to_str(const uint8_t addr[DS_HCI_BD_ADDR_LEN], char *str) {
  int i;
  if (addr == NULL || str == NULL) return;

  for (i = 0; i < DS_HCI_BD_ADDR_LEN; i++) {
    uint8_t b = addr[DS_HCI_BD_ADDR_LEN - 1 - i];   /* wire order is reversed */
    str[i * 3] = HEX[b >> 4];
    str[i * 3 + 1] = HEX[b & 0x0F];
    if (i != DS_HCI_BD_ADDR_LEN - 1) str[i * 3 + 2] = ':';
  }
  str[17] = '\0';
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool ds_hci_str_to_addr(const char *str, uint8_t addr[DS_HCI_BD_ADDR_LEN]) {
  int i;
  if (str == NULL || addr == NULL) return false;

  for (i = 0; i < DS_HCI_BD_ADDR_LEN; i++) {
    int hi = hex_digit(str[i * 3]);
    int lo = hex_digit(str[i * 3 + 1]);
    if (hi < 0 || lo < 0) return false;
    if (i != DS_HCI_BD_ADDR_LEN - 1) {
      char sep = str[i * 3 + 2];
      if (sep != ':' && sep != '-') return false;
    }
    addr[DS_HCI_BD_ADDR_LEN - 1 - i] = (uint8_t)((hi << 4) | lo);
  }
  return str[17] == '\0';
}

const char *ds_hci_error_name(uint8_t code) {
  switch (code) {
    case 0x00: return "success";
    case 0x04: return "page timeout";
    case 0x05: return "authentication failure";
    case 0x06: return "pin or key missing";
    case 0x08: return "connection timeout";
    case 0x0C: return "command disallowed";
    case 0x13: return "remote user ended it";
    case 0x14: return "remote out of resources";
    case 0x15: return "remote powered off";
    case 0x16: return "ended by the local host";
    case 0x1A: return "unsupported remote feature";
    case 0x22: return "link layer response timeout";
    case 0x28: return "instant passed";
    case 0x3B: return "unacceptable connection parameters";
    case 0x3D: return "terminated for message integrity failure";
    default: {
      /* "unknown" on its own has told us nothing twice now. One caller at a
       * time formats and prints immediately, so a shared buffer is enough. */
      static char other[16];
      static const char hexd[] = "0123456789ABCDEF";
      other[0] = 'e'; other[1] = 'r'; other[2] = 'r'; other[3] = ' ';
      other[4] = '0'; other[5] = 'x';
      other[6] = hexd[(code >> 4) & 0x0F];
      other[7] = hexd[code & 0x0F];
      other[8] = 0;
      return other;
    }
  }
}

const char *ds_hci_event_name(uint8_t code) {
  switch (code) {
    case DS_HCI_EVT_INQUIRY_COMPLETE:          return "inquiry complete";
    case DS_HCI_EVT_INQUIRY_RESULT:            return "inquiry result";
    case DS_HCI_EVT_CONNECTION_COMPLETE:       return "connection complete";
    case DS_HCI_EVT_CONNECTION_REQUEST:        return "connection request";
    case DS_HCI_EVT_DISCONNECTION_COMPLETE:    return "disconnection complete";
    case DS_HCI_EVT_AUTHENTICATION_COMPLETE:   return "authentication complete";
    case DS_HCI_EVT_REMOTE_NAME_COMPLETE:      return "remote name complete";
    case DS_HCI_EVT_ENCRYPTION_CHANGE:         return "encryption change";
    case DS_HCI_EVT_REMOTE_FEATURES_COMPLETE:  return "remote features";
    case 0x0C:                                  return "remote version";
    case DS_HCI_EVT_QOS_SETUP_COMPLETE:        return "qos setup complete";
    case DS_HCI_EVT_COMMAND_COMPLETE:          return "command complete";
    case DS_HCI_EVT_COMMAND_STATUS:            return "command status";
    case DS_HCI_EVT_NUM_COMPLETED_PACKETS:     return "number of completed packets";
    case DS_HCI_EVT_LINK_KEY_REQUEST:          return "link key request";
    case DS_HCI_EVT_LINK_KEY_NOTIFICATION:     return "link key notification";
    case DS_HCI_EVT_REMOTE_EXT_FEATURES:       return "remote ext features";
    case DS_HCI_EVT_ROLE_CHANGE:               return "role change";
    case DS_HCI_EVT_MODE_CHANGE:               return "mode change";
    case DS_HCI_EVT_MAX_SLOTS_CHANGE:          return "max slots change";
    case 0x20:                                  return "page scan mode change";
    case 0x30:                                  return "encryption key refresh";
    case 0x2E:                                  return "sniff subrating";
    case 0x38:                                  return "supervision timeout changed";
    case DS_HCI_EVT_INQUIRY_RESULT_RSSI:       return "inquiry result with rssi";
    case DS_HCI_EVT_EXTENDED_INQUIRY_RESULT:   return "extended inquiry result";
    case DS_HCI_EVT_IO_CAPABILITY_REQUEST:     return "io capability request";
    case DS_HCI_EVT_IO_CAPABILITY_RESPONSE:    return "io capability response";
    case DS_HCI_EVT_USER_CONFIRMATION_REQUEST: return "user confirmation request";
    case DS_HCI_EVT_SIMPLE_PAIRING_COMPLETE:   return "simple pairing complete";
    default:                                    return "unknown";
  }
}

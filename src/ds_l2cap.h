/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_l2cap.h — just enough L2CAP to carry HID over Bluetooth Classic.
 *
 * A DualSense speaks over two L2CAP channels: PSM 0x0011 for HID control and
 * PSM 0x0013 for HID interrupt, which is where the input reports and the
 * output reports actually flow. This layer opens them, configures them, keeps
 * them, and hands the payloads up.
 *
 * It sits directly on ds_hci_host: wire `send` to ds_hci_host_send_l2cap
 * and feed ds_l2cap_handle from the host's on_l2cap callback. As with
 * everything below it, nothing blocks, allocates or reads a clock, so the
 * whole channel handshake can be replayed on a desk.
 *
 * Deliberately not implemented, because a DualSense never asks for any of it:
 * enhanced retransmission mode, streaming mode, connectionless data, and
 * anything above two simultaneous channels. Unknown configuration options are
 * rejected properly rather than ignored, so a peer that wants those finds out
 * instead of hanging.
 *
 * Source: Bluetooth Core Specification, Vol 3 Part A (L2CAP).
 */
#ifndef DS_L2CAP_H
#define DS_L2CAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*                            C O N S T A N T S                               */
/* ========================================================================== */

#define DS_L2CAP_CID_SIGNALLING    0x0001
#define DS_L2CAP_CID_DYNAMIC_FIRST 0x0040  /* below this is reserved */

#define DS_L2CAP_PSM_SDP           0x0001
#define DS_L2CAP_PSM_HID_CONTROL   0x0011
#define DS_L2CAP_PSM_HID_INTERRUPT 0x0013

/* Signalling command codes */
#define DS_L2CAP_CMD_REJECT        0x01
#define DS_L2CAP_CONN_REQ          0x02
#define DS_L2CAP_CONN_RSP          0x03
#define DS_L2CAP_CONFIG_REQ        0x04
#define DS_L2CAP_CONFIG_RSP        0x05
#define DS_L2CAP_DISCONN_REQ       0x06
#define DS_L2CAP_DISCONN_RSP       0x07
#define DS_L2CAP_ECHO_REQ          0x08
#define DS_L2CAP_ECHO_RSP          0x09
#define DS_L2CAP_INFO_REQ          0x0A
#define DS_L2CAP_INFO_RSP          0x0B

/* Connection Response results */
#define DS_L2CAP_CONN_SUCCESS      0x0000
#define DS_L2CAP_CONN_PENDING      0x0001
#define DS_L2CAP_CONN_NO_PSM       0x0002
#define DS_L2CAP_CONN_SECURITY     0x0003
#define DS_L2CAP_CONN_NO_RESOURCES 0x0004

/* Configuration Response results */
#define DS_L2CAP_CFG_SUCCESS       0x0000
#define DS_L2CAP_CFG_UNACCEPTABLE  0x0001
#define DS_L2CAP_CFG_REJECTED      0x0002
#define DS_L2CAP_CFG_UNKNOWN_OPT   0x0003

/* Configuration option types. The top bit marks an option as a hint, which
 * the receiver is allowed to ignore rather than reject. */
#define DS_L2CAP_OPT_MTU           0x01
#define DS_L2CAP_OPT_FLUSH_TIMEOUT 0x02
#define DS_L2CAP_OPT_QOS           0x03
#define DS_L2CAP_OPT_RETRANSMIT    0x04
#define DS_L2CAP_OPT_HINT          0x80

/* The specification default, and plenty: the largest thing a DualSense sends
 * is a 79 byte report. */
#define DS_L2CAP_DEFAULT_MTU       672

/* Control and interrupt. There is no third channel to open. */
/* HID control, HID interrupt, and the service discovery channel a pad opens
 * on its way in. Two was enough right up until the pad did the paging. */
#define DS_L2CAP_MAX_CHANNELS      3

/* ========================================================================== */
/*                              C H A N N E L S                               */
/* ========================================================================== */

typedef enum {
  DS_L2CAP_CLOSED = 0,
  DS_L2CAP_CONNECTING,   /* connection request out, no response yet     */
  DS_L2CAP_CONFIGURING,  /* connected; options being agreed both ways   */
  DS_L2CAP_OPEN,         /* usable                                      */
} ds_l2cap_chan_state_t;

const char *ds_l2cap_state_name(ds_l2cap_chan_state_t st);

typedef struct {
  uint16_t psm;
  uint16_t local_cid;
  uint16_t remote_cid;
  ds_l2cap_chan_state_t state;
  /* A channel opens only when both directions have agreed. Tracking them
   * separately matters: the peer may configure us before we configure it,
   * and treating either half as the whole thing opens the channel early. */
  bool config_sent_ok;    /* our request was accepted   */
  bool config_recv_ok;    /* we accepted their request  */
  bool config_retried;    /* we already gave way on the MTU once */
  uint16_t remote_mtu;    /* the most they will accept from us */
} ds_l2cap_chan_t;

/* ========================================================================== */
/*                            C O N F I G U R A T I O N                       */
/* ========================================================================== */

typedef struct {
  /* The most we can receive in one packet. Zero means the specification
   * default of 672. Do not advertise more than the reassembly buffer
   * underneath can hold. */
  uint16_t mtu;

  /* Puts an L2CAP payload on the given channel id. Wire this straight to
   * ds_hci_host_send_l2cap. Return 0 on success. */
  int (*send)(void *ctx, uint16_t cid, const uint8_t *data, uint16_t len);

  /* A channel finished configuring, or went away. */
  void (*on_open)(void *ctx, uint16_t psm);
  void (*on_close)(void *ctx, uint16_t psm);

  /* Payload on an open channel. For the interrupt PSM this is exactly what
   * ds_find_bt_report wants: a 0xA1 transaction byte and a report. */
  void (*on_data)(void *ctx, uint16_t psm, const uint8_t *data, uint16_t len);

  void (*log)(void *ctx, const char *msg);
  void *ctx;
} ds_l2cap_config_t;

typedef struct {
  ds_l2cap_config_t cfg;
  ds_l2cap_chan_t chan[DS_L2CAP_MAX_CHANNELS];
  uint8_t next_ident;
  uint16_t next_cid;
  /* Set by ds_l2cap_connect_hid: open the interrupt channel by itself once
   * control is up, because some devices refuse interrupt before control. */
  bool auto_hid;
  uint8_t tx[96];         /* signalling packets are small */
} ds_l2cap_t;

/* cfg is copied. Returns false without a send function. */
bool ds_l2cap_init(ds_l2cap_t *l, const ds_l2cap_config_t *cfg);

/* Forget every channel without saying anything on the wire. Call this when
 * the ACL link drops: the channels are already gone with it. */
void ds_l2cap_reset(ds_l2cap_t *l);

/* --- opening -------------------------------------------------------------- */

/* Opens one channel. on_open fires when it is usable. */
bool ds_l2cap_connect(ds_l2cap_t *l, uint16_t psm);

/* The normal thing to call: control first, then interrupt by itself once
 * control is open. on_open fires twice, interrupt last. */
bool ds_l2cap_connect_hid(ds_l2cap_t *l);

bool ds_l2cap_disconnect(ds_l2cap_t *l, uint16_t psm);

/* --- traffic -------------------------------------------------------------- */

/* Feed every packet the layer below hands up, signalling included. */
void ds_l2cap_handle(ds_l2cap_t *l, uint16_t cid, const uint8_t *data,
                      uint16_t len);

/* Returns false when the channel is not open or the payload is longer than
 * the peer agreed to accept. */
bool ds_l2cap_send(ds_l2cap_t *l, uint16_t psm, const uint8_t *data,
                    uint16_t len);

/* --- asking --------------------------------------------------------------- */

bool ds_l2cap_is_open(const ds_l2cap_t *l, uint16_t psm);

/* True once both HID channels are open, which is the point at which a
 * DualSense is actually usable. */
bool ds_l2cap_hid_ready(const ds_l2cap_t *l);

/* NULL when no channel is using that PSM. */
const ds_l2cap_chan_t *ds_l2cap_channel(const ds_l2cap_t *l, uint16_t psm);

#ifdef __cplusplus
}
#endif

#endif /* DS_L2CAP_H */

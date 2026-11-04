/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_hci_host.h — the HCI conversation: bring a controller up, find a
 * DualSense, page it, pair with it, and keep an encrypted ACL link.
 *
 * ds_hci.c knows what a packet looks like. This knows what order to send
 * them in and what to say back. It still knows nothing about USB: you give it
 * two functions that put bytes on the wire, and you hand it every event and
 * ACL packet that comes off it. Everything else is internal.
 *
 *     ds_hci_host_init(&host, &cfg);
 *     ds_hci_host_start(&host);              // runs the init sequence
 *     ...feed events as they arrive...
 *     // on_state says DS_HOST_READY
 *     ds_hci_host_connect(&host, addr);
 *     // on_state says DS_HOST_CONNECTED, on_l2cap starts firing
 *
 * Nothing here blocks, allocates or knows what time it is. That is deliberate:
 * with the wire replaced by a recording function, the whole connect-and-pair
 * conversation can be replayed and checked byte for byte with no hardware
 * attached. See examples/SelfTest/HciHost.
 */
#ifndef DS_HCI_HOST_H
#define DS_HCI_HOST_H

#include "ds_hci.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scratch space for outgoing ACL. Fragments are capped at this minus the
 * 4 byte header, or at whatever the controller said it can take, whichever is
 * smaller. A DualSense report is 79 bytes, so the default never fragments. */
#ifndef DS_HCI_HOST_TX_MAX
#define DS_HCI_HOST_TX_MAX 256
#endif

/* ========================================================================== */
/*                                  S T A T E                                 */
/* ========================================================================== */

typedef enum {
  DS_HOST_IDLE = 0,       /* nothing started                              */
  DS_HOST_INITIALISING,   /* walking the controller setup sequence        */
  DS_HOST_READY,          /* controller configured, no link               */
  DS_HOST_INQUIRING,      /* scanning; on_device_found fires as they reply */
  DS_HOST_CONNECTING,     /* paging, then authenticating and encrypting   */
  DS_HOST_CONNECTED,      /* ACL up and encrypted; on_l2cap is live       */
  DS_HOST_FAILED,         /* gave up; call start() again to retry         */
} ds_hci_host_state_t;

const char *ds_hci_host_state_name(ds_hci_host_state_t st);

/* ========================================================================== */
/*                            C O N F I G U R A T I O N                       */
/* ========================================================================== */

typedef struct {
  /* --- identity ---
   * Every field here treats zero as "use the default", so a zeroed config
   * struct is a working one. Note what that costs on the two fields where
   * zero is also a legal value: you cannot select DS_HCI_IO_DISPLAY_ONLY or
   * DS_HCI_AUTH_NO_MITM_NO_BONDING, because both are 0x00 and both are the
   * wrong answer for a gamepad anyway — there is no display to show a code
   * on, and refusing to bond makes the DualSense forget you every session. */
  const char *name;             /* local name, default "DualSense32"        */
  uint32_t class_of_device;     /* default 0x000104, computer/handheld      */
  uint8_t io_capability;        /* default DS_HCI_IO_NO_INPUT_OUTPUT       */
  uint8_t auth_requirements;    /* default DS_HCI_AUTH_NO_MITM_GENERAL     */
  uint8_t scan_enable;          /* default DS_HCI_SCAN_PAGE, so a bonded
                                 * DualSense can reconnect to us on its own */

  /* --- the wire. Both must be set. Return 0 on success. --- */
  int (*send_cmd)(void *ctx, const uint8_t *pkt, uint16_t len);
  int (*send_acl)(void *ctx, const uint8_t *pkt, uint16_t len);

  /* --- what comes back up. All optional. --- */

  void (*on_state)(void *ctx, ds_hci_host_state_t st);

  /* One inquiry response. Call ds_hci_host_connect from here or later; the
   * inquiry is cancelled automatically when you do. */
  void (*on_device_found)(void *ctx, const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                          uint32_t class_of_device, int8_t rssi, bool has_rssi);

  /* A complete, reassembled L2CAP packet with its header already stripped. */
  void (*on_l2cap)(void *ctx, uint16_t cid, const uint8_t *data, uint16_t len);

  /* Asked when the remote wants to skip pairing. Fill `key` and return true
   * if you have one stored for this address, false to pair afresh. Leaving
   * this NULL means every connection pairs from scratch, which works but
   * makes the DualSense forget us between sessions. */
  bool (*get_link_key)(void *ctx, const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                       uint8_t key[DS_HCI_LINK_KEY_LEN]);

  /* Asked when something pages us. Return false to turn it away before any
   * pairing can start. Leaving this NULL accepts every caller, which is right
   * until a pad is bonded and wrong after: a stranger that pairs takes the
   * one bond slot, and the real pad fails authentication from then on. */
  bool (*accept_peer)(void *ctx, const uint8_t addr[DS_HCI_BD_ADDR_LEN]);

  /* A new key was negotiated. Persist it and hand it back from get_link_key
   * next time. */
  void (*on_link_key)(void *ctx, const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                      const uint8_t key[DS_HCI_LINK_KEY_LEN]);

  /* Short fixed strings, no formatting. Wire it to Serial.println. */
  void (*log)(void *ctx, const char *msg);

  void *ctx;
} ds_hci_host_config_t;

/* ========================================================================== */
/*                                  H O S T                                   */
/* ========================================================================== */

typedef struct {
  ds_hci_host_config_t cfg;
  ds_hci_host_state_t state;

  uint8_t init_step;            /* index into the setup sequence      */
  uint16_t pending_opcode;      /* the command we are waiting to hear about */

  /* Learned from the controller during setup. */
  uint8_t hci_version;
  uint16_t manufacturer;
  uint8_t local_addr[DS_HCI_BD_ADDR_LEN];
  uint16_t acl_mtu;             /* largest ACL payload it will accept */
  uint16_t acl_credits_total;
  uint16_t acl_credits;         /* how many we may send right now     */

  /* The current link. */
  uint8_t peer_addr[DS_HCI_BD_ADDR_LEN];
  uint16_t handle;
  bool have_handle;
  bool encrypted;
  /* True when the peer paged us rather than the other way round.
   * Informational: a DualSense pages the host when PS is pressed but then
   * waits to be asked, so the channels still have to be opened from this end
   * either way. Measured, not assumed — waiting for it to open them means
   * waiting for ever. */
  bool inbound;
  /* The reason byte from the last Disconnection Complete. When a link
   * ends on its own this is the only thing that says whether the peer
   * chose to go, the radio lost it, or we ended it ourselves. */
  uint8_t last_disconnect_reason;
  /* Status from the last Connection Complete. A failed page says why
   * here and nowhere else. */
  uint8_t last_conn_status;
  /* Status from the last Authentication Complete. 0x05 means the keys
   * disagree; 0x06 means the peer has no key for us at all, which is a
   * different problem with a different fix. */
  uint8_t last_auth_status;
  /* Key type from the last Link Key Notification. 0x06 is a changed
   * combination key — the peer replacing one we already had. */
  uint8_t last_key_type;
  /* Authentication is started from whichever feature read comes back first,
   * and both may. */
  bool authenticating;
  /* True once a Role Change says this end is the central. Only then may the
   * supervision timeout be written. */
  bool is_central;

  ds_hci_reasm_t reasm;
  uint8_t tx[DS_HCI_HOST_TX_MAX];
} ds_hci_host_t;

/* cfg is copied, so it may live on the stack. Returns false if send_cmd or
 * send_acl is missing. */
bool ds_hci_host_init(ds_hci_host_t *h, const ds_hci_host_config_t *cfg);

/* --- driving ------------------------------------------------------------- */

/* Sends HCI_Reset and starts the setup sequence. Legal from any state; a
 * controller that has just been plugged in needs this before anything else. */
bool ds_hci_host_start(ds_hci_host_t *h);

/* Scans for `seconds` (rounded to the controller's 1.28 s units, so 1-61).
 * Allowed once the controller is set up and there is no link in progress,
 * which means from DS_HOST_READY or DS_HOST_FAILED. */
bool ds_hci_host_inquiry(ds_hci_host_t *h, uint8_t seconds);
bool ds_hci_host_inquiry_cancel(ds_hci_host_t *h);

/* Pages `addr`, then authenticates and encrypts without further prompting.
 * Cancels a running inquiry first. */
bool ds_hci_host_connect(ds_hci_host_t *h,
                          const uint8_t addr[DS_HCI_BD_ADDR_LEN]);

bool ds_hci_host_disconnect(ds_hci_host_t *h);

/* --- feeding -------------------------------------------------------------
 *
 * Hand these every packet the transport pulls off the interrupt and bulk
 * endpoints, in arrival order. Packets that are not ours are ignored, so it
 * is safe to feed everything. */
void ds_hci_host_handle_event(ds_hci_host_t *h, const uint8_t *pkt,
                               uint16_t len);
void ds_hci_host_handle_acl(ds_hci_host_t *h, const uint8_t *pkt,
                             uint16_t len);

/* --- sending -------------------------------------------------------------
 *
 * Wraps an L2CAP packet in as many ACL fragments as the controller's buffer
 * size demands and sends them. `data` is the L2CAP payload; the 4 byte L2CAP
 * header is written for you.
 *
 * Returns false when there is no link, or when the controller has no buffer
 * free. There is no queue: at DualSense report rates the credits never run
 * out, and silently buffering output would only add latency to a control
 * path where latency is the whole point. */
bool ds_hci_host_send_l2cap(ds_hci_host_t *h, uint16_t cid,
                             const uint8_t *data, uint16_t len);

/* --- asking --------------------------------------------------------------- */

static inline ds_hci_host_state_t ds_hci_host_state(const ds_hci_host_t *h) {
  return h->state;
}

static inline bool ds_hci_host_is_connected(const ds_hci_host_t *h) {
  return h->state == DS_HOST_CONNECTED;
}

/* Bluetooth major device class 5 with minor 2: a gamepad. Both DualShock 4
 * and DualSense report 0x002508. Useful for filtering inquiry results, but
 * treat it as a hint — check the name if you need certainty. */
bool ds_hci_cod_is_gamepad(uint32_t class_of_device);

#ifdef __cplusplus
}
#endif

#endif /* DS_HCI_HOST_H */

/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_transport.h — the seam between the protocol core and whatever actually
 * moves bytes to and from the controller.
 *
 * A transport has exactly three jobs:
 *   1. get connected, and say when it is
 *   2. hand every inbound HID input report to the core
 *   3. put an outbound report on the wire
 *
 * Everything about *what* those bytes mean lives in ds_protocol.c, so adding
 * a link means implementing this interface and nothing else.
 *
 * The implementations:
 *   ds_bt_bluedroid    original ESP32, Bluetooth Classic over L2CAP
 *   ds_link_usb_hci    ESP32-S3, USB Bluetooth dongle as an HCI radio
 *   ds_link_usb_hid    ESP32-S3, a pad plugged straight in
 *
 * All three carry either controller.
 */
#ifndef DS_TRANSPORT_H
#define DS_TRANSPORT_H

#include "ds_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  DS_DISCONNECTED = 0,
  DS_CONNECTING,
  DS_CONNECTED,
  DS_LINK_ERROR,
} ds_conn_state_t;

/* Called from the transport's own context — usually a Bluetooth or USB task,
 * not loop(). Keep them short and do not block: the original library stalled
 * the whole L2CAP queue with a 250 ms delay in one of these. */
typedef void (*ds_report_cb_t)(const uint8_t *report, uint16_t len, void *user);
typedef void (*ds_state_cb_t)(ds_conn_state_t state, void *user);

typedef struct ds_transport ds_transport_t;

struct ds_transport {
  /* Which report dialect this link speaks; the core needs it for every call. */
  ds_link_t link;

  /* And which controller is on the end of it. A link that cannot tell — one
   * that never sees a USB product id, say — leaves this at
   * DS_DEVICE_DUALSENSE. Set before on_state reports DS_CONNECTED, so a
   * sketch may read it as soon as it is connected. */
  ds_device_t device;

  /* Whether anything has actually said so. False leaves `device` at its
   * default, which is a guess and not an answer — over Bluetooth the pad is
   * only named by its first full report, and there is always a window after
   * connecting where nothing has named it. Code that behaves differently per
   * controller should wait for this rather than read the default. */
  bool device_named;

  /* What the sketch said to expect, from DualSense32::begin(DS_DEVICE_...).
   *
   * This is a starting value and nothing more. It fills the window between
   * connecting and being named -- which only exists over Bluetooth, where the
   * pad is identified by its first full report -- so that output built during
   * it goes out in the right dialect instead of the default one.
   *
   * It never outranks a real identification. A USB product id or a Bluetooth
   * report id replaces it and sets device_named; if the two disagree, that
   * disagreement is exactly what DualShock4::mismatched() reports. The one
   * thing that does outrank identification is ds_link_usb_hid_force_device(),
   * which exists for clones whose product id is wrong to begin with. */
  ds_device_t device_expect;
  bool device_expected;

  /* Bring the link up. `addr` is transport-specific and may be NULL:
   * a Bluetooth address for the BT links, ignored for direct USB. */
  bool (*begin)(ds_transport_t *self, const uint8_t addr[6]);
  void (*end)(ds_transport_t *self);

  /* Optional pump for transports that cannot run themselves off an interrupt
   * or task. May be NULL. Safe to call from loop() as often as you like. */
  void (*poll)(ds_transport_t *self);

  /* Send one output report, already framed and signed by ds_build_output().
   * Returns false if the link is down or the queue is full. */
  bool (*send)(ds_transport_t *self, const uint8_t *report, uint16_t len);

  ds_conn_state_t (*state)(ds_transport_t *self);

  /* The pad's factory motion calibration, or NULL if it has not been read
   * yet or the link has no way to ask. Every link fetches this for itself;
   * having it here is what lets one sketch call ds_apply_calibration()
   * without knowing which link it is running over. May be NULL. */
  const ds_calibration_t *(*calibration)(ds_transport_t *self);

  /* The controller on the other end, six bytes in HCI wire order — the
   * reverse of how an address is written. NULL when nothing is connected or
   * the link has no way to know. Optional: check it before calling. */
  const uint8_t *(*peer)(ds_transport_t *self);

  /* Set by the owner before begin(). */
  ds_report_cb_t on_report;
  ds_state_cb_t on_state;
  void *user;

  /* Transport-private data. */
  void *impl;
};

/* Back to knowing nothing, which is where a link starts and where it returns
 * on disconnect. Without this a pad that unplugs leaves its identity behind,
 * and the next one to connect is confidently called by the last one's name
 * until its first full report arrives. */
static inline void ds_transport_forget_device(ds_transport_t *tr) {
  tr->device = tr->device_expected ? tr->device_expect : DS_DEVICE_DUALSENSE;
  tr->device_named = false;
}

/* ---------------------------------------------------------------------------
 * Sequence tags
 *
 * The Bluetooth output report carries a 4-bit sequence number that the CRC32
 * covers, so it cannot be stamped into an already-built buffer. Advance
 * ds_output_t::sequence before calling ds_build_output() instead:
 *
 *   out.sequence = seq++;
 *   uint16_t n = ds_build_output(&out, DS_LINK_BT, buf, sizeof buf);
 *   tr->send(tr, buf, n);
 * ------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* DS_TRANSPORT_H */

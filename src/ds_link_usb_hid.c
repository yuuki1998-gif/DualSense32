/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_link_usb_hid.c — a DualSense on a wire, behind the transport seam.
 * See ds_link_usb_hid.h.
 */
#include "ds_link_usb_hid.h"

#ifdef DS_HAS_USB_HID

#include <string.h>

static struct {
  ds_transport_t tr;
  ds_calibration_t cal;
  uint8_t addr[6];
  bool have_addr;
  bool started;
  bool attached;
  bool forced;              /* the sketch overrode the product id */
  ds_device_t forced_dev;
} g;

static void announce(ds_conn_state_t st) {
  if (g.tr.on_state != NULL) g.tr.on_state(st, g.tr.user);
}

/* The product id is the only thing that distinguishes the two pads on this
 * link: both answer to input report 0x01 at 64 bytes, so by the time a report
 * arrives it is too late to ask. Anything Sony makes that is not a known
 * DualShock 4 is treated as a DualSense, which is what the filter in
 * ds_usb_hid.c lets through anyway. */
static ds_device_t device_for(uint16_t pid) {
  switch (pid) {
    case DS_USB_PID_DUALSHOCK4_V1:
    case DS_USB_PID_DUALSHOCK4_V2:
    case DS_USB_PID_DUALSHOCK4_DONGLE:
      return DS_DEVICE_DUALSHOCK4;
    default:
      return DS_DEVICE_DUALSENSE;
  }
}

/* Read once, on attach. Both of these live behind the control pipe, which is
 * only reachable while the device is claimed, and both hide behind a
 * different report id on a DualShock 4. The DualSense calibration report is
 * the longer of the two, so one buffer covers both. */
static void read_features(void) {
  uint8_t buf[DS5_FEATURE_CALIBRATION_SIZE];
  const bool ds4 = g.tr.device == DS_DEVICE_DUALSHOCK4;
  int n;

  n = ds_usb_hid_get_feature(
      ds4 ? DS4_FEATURE_PAIRING_INFO : DS5_FEATURE_PAIRING_INFO, buf,
      ds4 ? DS4_FEATURE_PAIRING_INFO_SIZE : DS5_FEATURE_PAIRING_INFO_SIZE, 500);
  g.have_addr = n > 0 &&
                ds_parse_pairing_info_dev(buf, (uint16_t)n, g.tr.device, g.addr);

  n = ds_usb_hid_get_feature(
      ds4 ? DS4_FEATURE_CALIBRATION : DS5_FEATURE_CALIBRATION, buf,
      ds4 ? DS4_FEATURE_CALIBRATION_SIZE : DS5_FEATURE_CALIBRATION_SIZE, 500);
  if (n <= 0 || !ds_parse_calibration_dev(buf, (uint16_t)n, DS_LINK_USB,
                                           g.tr.device, &g.cal)) {
    memset(&g.cal, 0, sizeof(g.cal));
  }
}

static void on_report(void *ctx, const uint8_t *report, uint16_t len) {
  (void)ctx;
  if (g.tr.on_report != NULL) g.tr.on_report(report, len, g.tr.user);
}

static void on_attach(void *ctx, uint16_t vid, uint16_t pid) {
  (void)ctx; (void)vid;
  /* Before read_features, which asks for different reports per device, and
   * well before DS_CONNECTED, which is when a sketch may read it. */
  g.tr.device = g.forced ? g.forced_dev : device_for(pid);
  g.tr.device_named = true;   /* the product id is an answer, not a guess */
  g.attached = true;
  read_features();
  announce(DS_CONNECTED);
}

static void on_detach(void *ctx) {
  (void)ctx;
  g.attached = false;
  g.have_addr = false;
  /* A forced device is an assertion about a clone whose product id cannot be
   * trusted, so it survives the unplug. Anything else goes back to not
   * knowing -- which is the sketch's expectation if it gave one. */
  if (g.forced) {
    g.tr.device = g.forced_dev;
    g.tr.device_named = true;
  } else {
    ds_transport_forget_device(&g.tr);
  }
  memset(&g.cal, 0, sizeof(g.cal));
  announce(DS_DISCONNECTED);
}

/* ------------------------------------------------------ transport methods */

static bool link_begin(ds_transport_t *self, const uint8_t addr[6]) {
  ds_usb_hid_config_t cfg;
  (void)self;
  (void)addr;   /* a wire has one thing on the end of it */

  if (g.started) return true;
  memset(&cfg, 0, sizeof(cfg));
  cfg.on_report = on_report;
  cfg.on_attach = on_attach;
  cfg.on_detach = on_detach;
  if (!ds_usb_hid_init(&cfg)) return false;

  g.started = true;
  announce(DS_CONNECTING);
  return true;
}

static void link_end(ds_transport_t *self) {
  (void)self;
  if (!g.started) return;
  ds_usb_hid_deinit();
  g.started = false;
  g.attached = false;
  announce(DS_DISCONNECTED);
}

static void link_poll(ds_transport_t *self) {
  (void)self;
  if (g.started) ds_usb_hid_poll();
}

static bool link_send(ds_transport_t *self, const uint8_t *report,
                      uint16_t len) {
  (void)self;
  return ds_usb_hid_send(report, len);
}

static ds_conn_state_t link_state(ds_transport_t *self) {
  (void)self;
  if (!g.started) return DS_DISCONNECTED;
  return g.attached ? DS_CONNECTED : DS_CONNECTING;
}

/* ----------------------------------------------------------------- public */

ds_transport_t *ds_link_usb_hid(void) {
  if (g.tr.begin == NULL) {
    g.tr.link = DS_LINK_USB;
    g.tr.device = DS_DEVICE_DUALSENSE;   /* until something attaches */
    g.tr.begin = link_begin;
    g.tr.end = link_end;
    g.tr.poll = link_poll;
    g.tr.send = link_send;
    g.tr.state = link_state;
    g.tr.calibration = ds_link_usb_hid_calibration;
  g.tr.peer = ds_link_usb_hid_address;
  }
  return &g.tr;
}

const ds_calibration_t *ds_link_usb_hid_calibration(ds_transport_t *tr) {
  (void)tr;
  return g.cal.valid ? &g.cal : NULL;
}

const uint8_t *ds_link_usb_hid_address(ds_transport_t *tr) {
  (void)tr;
  return g.have_addr ? g.addr : NULL;
}

ds_device_t ds_link_usb_hid_device(ds_transport_t *tr) {
  (void)tr;
  return g.tr.device;
}

void ds_link_usb_hid_force_device(ds_device_t dev) {
  g.forced = true;
  g.forced_dev = dev;
  if (g.attached) g.tr.device = dev;   /* take effect without a replug */
}

void ds_link_usb_hid_detect_device(void) {
  g.forced = false;
}

int ds_link_usb_hid_feature(ds_transport_t *tr, uint8_t report_id,
                             uint8_t *buf, uint16_t len, uint32_t timeout_ms) {
  (void)tr;
  return ds_usb_hid_get_feature(report_id, buf, len, timeout_ms);
}

#endif /* DS_HAS_USB_HID */

/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_link_usb_hid.h — a DualSense on a wire, behind the common transport
 * seam.
 *
 *     ds_transport_t *tr = ds_link_usb_hid();
 *     tr->on_report = onReport;
 *     tr->on_state  = onState;
 *     tr->begin(tr, NULL);          // no address: there is only one wire
 *     ...
 *     void loop() { tr->poll(tr); }
 *
 * Note tr->link is DS_LINK_USB here, so pass that to ds_parse_input and
 * ds_build_output. The report ids, the field order and the framing all
 * differ from Bluetooth, and using the wrong one decodes to plausible
 * nonsense rather than failing.
 *
 * Hardware: an ESP32-S3 or S2 with the controller on its USB host port.
 */
#ifndef DS_LINK_USB_HID_H
#define DS_LINK_USB_HID_H

#include "ds_transport.h"
#include "ds_usb_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DS_HAS_USB_HID

/* One controller, one transport. Never returns NULL. `addr` to begin() is
 * ignored: a wire has only one thing on the end of it. */
ds_transport_t *ds_link_usb_hid(void);

/* Factory motion calibration, read once on attach. NULL if the pad did not
 * answer. Pass it to ds_apply_calibration() — raw gyro is about sixty times
 * smaller than calibrated gyro, so this is not a fine adjustment. */
const ds_calibration_t *ds_link_usb_hid_calibration(ds_transport_t *tr);

/* The controller's own Bluetooth address, read from feature report 0x09 on
 * attach, in HCI wire order. NULL if unread.
 *
 * This is the interesting part of the wired link even when you mean to use
 * Bluetooth: it identifies the pad without an inquiry, and the bytes go
 * straight to ds_hci_addr_to_str or a Create Connection. */
const uint8_t *ds_link_usb_hid_address(ds_transport_t *tr);

/* Which pad attached, worked out from the USB product id. DS_DEVICE_DUALSENSE
 * until something is on the wire. Also on tr->device, which is what the
 * protocol core wants. */
ds_device_t ds_link_usb_hid_device(ds_transport_t *tr);

/* Override the product id, for a pad this library has no entry for — a clone,
 * or a revision newer than this code. Takes effect immediately if something is
 * already attached, and survives a replug.
 *
 * You should not need this. Over USB the product id is authoritative, and over
 * Bluetooth the two pads announce different report ids (0x31 and 0x11), so the
 * link works it out either way. Reach for it when the log says a pad attached
 * and every field past the sticks reads as nonsense.
 *
 * ds_link_usb_hid_detect_device() goes back to trusting the product id. */
void ds_link_usb_hid_force_device(ds_device_t dev);
void ds_link_usb_hid_detect_device(void);

/* Any other feature report, for anyone who wants to go further. Waits for the
 * answer, up to timeout_ms. Returns bytes read, or -1. */
int ds_link_usb_hid_feature(ds_transport_t *tr, uint8_t report_id,
                             uint8_t *buf, uint16_t len, uint32_t timeout_ms);

#endif /* DS_HAS_USB_HID */

#ifdef __cplusplus
}
#endif

#endif /* DS_LINK_USB_HID_H */

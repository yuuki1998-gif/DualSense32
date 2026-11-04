/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_usb_hid.h — a DualSense plugged straight into a USB host.
 *
 * No radio, no pairing, no CRC: the pad appears as a plain HID device and its
 * reports arrive on an interrupt endpoint. That makes this the simplest way to
 * use a DualSense, and also the easiest way to ask it questions — feature
 * reports are how you read its factory motion calibration and its own
 * Bluetooth address.
 *
 *     interrupt IN   -> on_report()                input report 0x01, 64 bytes
 *     interrupt OUT  <- ds_usb_hid_send()         output report 0x02, 63 bytes
 *     control        <- ds_usb_hid_get_feature()  calibration, pairing info
 *
 * The DualSense also exposes three USB audio interfaces for its headset jack.
 * They are ignored; only the HID interface is claimed, which is found by class
 * rather than by number so a Edge or a later revision still works.
 *
 * This and ds_usb_hci are alternatives, not companions: each installs the USB
 * host library, and a board has one port. Use this one to talk to a pad on a
 * wire, that one to talk to a dongle.
 *
 * Measured against a DualSense CFI-ZCT1J on an ESP32-S3-DevKitC-1; see
 * VERIFICATION.md for the descriptor dump.
 */
#ifndef DS_USB_HID_H
#define DS_USB_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED
#define DS_HAS_USB_HID 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DS_HAS_USB_HID

#define DS_USB_VID_SONY           0x054C
#define DS_USB_PID_DUALSENSE      0x0CE6
#define DS_USB_PID_DUALSENSE_EDGE 0x0DF2

/* The DualShock 4s. Two hardware revisions and the official USB adaptor, all
 * speaking the same reports. The 2016 revision (ZCT2) is the one with the
 * light bar visible through the touchpad. */
#define DS_USB_PID_DUALSHOCK4_V1  0x05C4   /* CUH-ZCT1, 2013 */
#define DS_USB_PID_DUALSHOCK4_V2  0x09CC   /* CUH-ZCT2, 2016 */
#define DS_USB_PID_DUALSHOCK4_DONGLE 0x0BA0

typedef struct {
  /* Attach only to this device. Leave both zero to take the first Sony
   * controller that turns up. */
  uint16_t vid, pid;

  /* One input report, starting at its report id. Feed straight to
   * ds_parse_input with DS_LINK_USB. Runs from inside ds_usb_hid_poll(). */
  void (*on_report)(void *ctx, const uint8_t *report, uint16_t len);

  void (*on_attach)(void *ctx, uint16_t vid, uint16_t pid);
  void (*on_detach)(void *ctx);
  void (*log)(void *ctx, const char *msg);
  void *ctx;
} ds_usb_hid_config_t;

bool ds_usb_hid_init(const ds_usb_hid_config_t *cfg);
void ds_usb_hid_deinit(void);

/* Drives enumeration and the inbound endpoint. Nothing happens without it. */
void ds_usb_hid_poll(void);

bool ds_usb_hid_attached(void);

/* Sends an output report, report id first. Build it with ds_build_output()
 * and DS_LINK_USB, which produces the 63 bytes this wants. */
bool ds_usb_hid_send(const uint8_t *report, uint16_t len);

/* Reads a feature report over the control pipe, report id included in the
 * result. Returns the number of bytes read, or -1.
 *
 * Unlike everything else here this one waits, pumping the USB host library
 * until the answer arrives or `timeout_ms` passes. Feature reports are read
 * once at startup, not in a control loop, so blocking briefly is the honest
 * trade against making the caller run a state machine for two calls. */
int ds_usb_hid_get_feature(uint8_t report_id, uint8_t *buf, uint16_t len,
                            uint32_t timeout_ms);

#endif /* DS_HAS_USB_HID */

#ifdef __cplusplus
}
#endif

#endif /* DS_USB_HID_H */

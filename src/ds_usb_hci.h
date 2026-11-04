/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_usb_hci.h — HCI over a USB Bluetooth dongle, for chips with USB OTG.
 *
 * This is the bottom of the stack and the only part that knows about USB. It
 * finds a standard Bluetooth HCI device (class 0xE0 / 0x01 / 0x01), claims its
 * first interface and turns the four pipes into two functions and two
 * callbacks:
 *
 *     control transfer  <- ds_usb_hci_send_cmd()      HCI commands
 *     interrupt IN      -> on_event()                  HCI events
 *     bulk OUT          <- ds_usb_hci_send_acl()      ACL out
 *     bulk IN           -> on_acl()                    ACL in
 *
 * The second interface is SCO audio and is left alone.
 *
 * send_cmd and send_acl have exactly the signature ds_hci_host_config_t
 * wants, so wiring the two together is assignment:
 *
 *     hostCfg.send_cmd = ds_usb_hci_send_cmd;
 *     hostCfg.send_acl = ds_usb_hci_send_acl;
 *     usbCfg.on_event  = feed ds_hci_host_handle_event
 *     usbCfg.on_acl    = feed ds_hci_host_handle_acl
 *
 * Neither send blocks or fails for want of a free transfer: commands are
 * queued and drained by ds_usb_hci_poll(), because the control pipe only
 * carries one at a time and the layer above has no business knowing that.
 *
 * One dongle at a time. There is no reason to want two and pretending
 * otherwise would cost every caller a handle.
 *
 * Measured against a Barrot BT6.0 adapter on an ESP32-S3-DevKitC-1; see
 * VERIFICATION.md for the descriptor dump this was written against.
 */
#ifndef DS_USB_HCI_H
#define DS_USB_HCI_H

#include <stdbool.h>
#include <stdint.h>

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED
#define DS_HAS_USB_HCI 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DS_HAS_USB_HCI

typedef struct {
  /* Attach only to this device. Leave both zero to take the first standard
   * HCI class device that appears, which is what you usually want. */
  uint16_t vid, pid;

  /* A complete HCI event, without any packet type byte — USB does not use
   * one. Feed straight to ds_hci_host_handle_event. Runs from inside
   * ds_usb_hci_poll(). */
  void (*on_event)(void *ctx, const uint8_t *pkt, uint16_t len);

  /* One ACL packet, header included. Feed to ds_hci_host_handle_acl. */
  void (*on_acl)(void *ctx, const uint8_t *pkt, uint16_t len);

  /* One outbound packet has left. Credit-based stacks need this to know
   * they may hand over the next one; the hand-rolled host did not, so it is
   * allowed to be NULL. Runs from inside ds_usb_hci_poll(). */
  void (*on_sent)(void *ctx);

  /* The dongle is claimed and the pipes are running, or it went away. */
  void (*on_attach)(void *ctx, uint16_t vid, uint16_t pid);
  void (*on_detach)(void *ctx);

  void (*log)(void *ctx, const char *msg);
  void *ctx;
} ds_usb_hci_config_t;

/* Installs the USB host library and starts looking. Returns false if the
 * library will not start, which on a chip without USB OTG it never will. */
bool ds_usb_hci_init(const ds_usb_hci_config_t *cfg);
void ds_usb_hci_deinit(void);

/* Drives everything: enumeration, the inbound pipes and the command queue.
 * Call it from loop() or a task. Nothing happens without it. */
void ds_usb_hci_poll(void);

bool ds_usb_hci_attached(void);

/* Both return 0 on success and non-zero on failure, matching what
 * ds_hci_host_config_t expects. The ctx argument is ignored — it exists so
 * these can be assigned to those function pointers directly. */
int ds_usb_hci_send_cmd(void *ctx, const uint8_t *pkt, uint16_t len);
int ds_usb_hci_send_acl(void *ctx, const uint8_t *pkt, uint16_t len);

/* Whether one more packet of that kind would be accepted right now. A stack
 * that asks before every send needs these; one that queues and forgets does
 * not, which is why the sends above never fail for want of a buffer. */
bool ds_usb_hci_can_send_cmd(void);
bool ds_usb_hci_can_send_acl(void);

#endif /* DS_HAS_USB_HCI */

#ifdef __cplusplus
}
#endif

#endif /* DS_USB_HCI_H */

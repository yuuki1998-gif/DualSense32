/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_link_usb_hci.h — the dongle route behind the common transport seam.
 *
 * Four layers live under this one call: USB pipes, the HCI conversation, the
 * two HID L2CAP channels, and the report framing. If you only want a
 * DualSense to work, this is the whole interface:
 *
 *     ds_transport_t *tr = ds_link_usb_hci();
 *     tr->on_report = onReport;
 *     tr->on_state  = onState;
 *     tr->begin(tr, NULL);
 *     ...
 *     void loop() { tr->poll(tr); }
 *
 * Reach past it to ds_hci_host or ds_l2cap when you want to see the
 * conversation; examples/Troubleshooting/DongleStepByStep does exactly that.
 *
 * Hardware: an ESP32-S3 or S2 with a standard Bluetooth HCI dongle on its USB
 * host port. The chip needs no Bluetooth radio of its own — that is the point.
 */
#ifndef DS_LINK_USB_HCI_H
#define DS_LINK_USB_HCI_H

#include "ds_hci_host.h"
#include "ds_l2cap.h"
#include "ds_transport.h"
#include "ds_usb_hci.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DS_HAS_USB_HCI

/* One dongle, one pad, one transport. Never returns NULL. */
ds_transport_t *ds_link_usb_hci(void);

/* --- pairing -------------------------------------------------------------
 *
 * begin(tr, NULL) with a bond set   wait for the pad to call in. Press PS.
 * begin(tr, NULL) with no bond      scan, and pair with the first gamepad
 *                                   found. Hold Create then PS on the pad.
 * begin(tr, addr)                   only that pad, ever: anyone else calling in
 *                                   is refused before pairing. Pages it until
 *                                   bonded (hold Create then PS); once bonded,
 *                                   pages once at start-up and otherwise waits
 *                                   for PS, as the original ESP32 route does.
 *
 * Scanning is deliberately skipped once a bond is known: an inquiry stops the
 * controller answering pages, so a pad pressing PS would ring into silence.
 * With a bond, a call from any other address is refused as well. */

/* Bonds are kept in NVS automatically, under the namespace "ds5bond".
 *
 * This is not the library being opinionated: the key has to survive a reboot
 * or the pad needs pairing mode every time, and on this route there is nobody
 * else to keep it. On the original ESP32, Bluedroid quietly does the same
 * thing with its own bond store, which is why a Bluetooth-Classic sketch
 * never seems to need one.
 *
 * Turn it off to store the key somewhere else — a file, a server, a
 * hard-coded constant — and supply it with set_bond(). Call before begin(). */
/*
 * Bring the link up, naming the pad the way its address is written:
 * "AA:BB:CC:DD:EE:FF", or with dashes. NULL waits for whichever bonded pad
 * presses PS, which is what most sketches want.
 *
 * Returns false if the string is not an address, so a typo fails here and
 * says so. Handed over as six bytes it would look like any other address and
 * the only symptom would be a host that pages nothing, for ever.
 *
 * HCI carries BD_ADDR in the opposite order to the one humans write, and the
 * reversal happens in here.
 */
bool ds_link_usb_hci_begin(ds_transport_t *tr, const char *addr);

void ds_link_usb_hci_persist(ds_transport_t *tr, bool enable);

/* Hand back a bond kept from a previous session, or one that was stored
 * outside NVS. Call before begin(). */
void ds_link_usb_hci_set_bond(ds_transport_t *tr,
                               const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                               const uint8_t key[DS_HCI_LINK_KEY_LEN]);

/* Forget it, so the next begin() pairs from scratch. Note that the pad has to
 * be made to forget too — Create then PS — because a DualSense remembers one
 * host and a half-forgotten pairing fails authentication for ever. */
void ds_link_usb_hci_clear_bond(ds_transport_t *tr);

/* Fires when a new bond is made. Persist both halves and give them back
 * through set_bond next time; without that, every session needs pairing
 * mode again. */
void ds_link_usb_hci_on_bond(ds_transport_t *tr,
                              void (*cb)(void *user,
                                         const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                         const uint8_t key[DS_HCI_LINK_KEY_LEN]),
                              void *user);

/* --- extras --------------------------------------------------------------- */

/* Factory motion calibration, fetched automatically once the HID control
 * channel opens. NULL until the pad has answered. Pass it to
 * ds_apply_calibration() — without it the gyro is in raw units, which are
 * about sixty times smaller than the calibrated ones. */
const ds_calibration_t *ds_link_usb_hci_calibration(ds_transport_t *tr);

/* The address of the pad on the other end, in HCI wire order. NULL when
 * nothing is connected. */
const uint8_t *ds_link_usb_hci_peer(ds_transport_t *tr);

/* Short fixed strings from the layers underneath: the HCI conversation and
 * the channel handshake both narrate themselves, and a link that will not
 * come up says why in here and nowhere else. Wire it to Serial.println.
 *
 * Swallowing these was a mistake once already — a wrapper you cannot see
 * inside is only convenient while it works. */
void ds_link_usb_hci_on_log(ds_transport_t *tr,
                             void (*cb)(void *user, const char *msg),
                             void *user);

/* The layers underneath, for anyone who wants to watch or steer them. */
ds_hci_host_t *ds_link_usb_hci_host(ds_transport_t *tr);
ds_l2cap_t *ds_link_usb_hci_l2cap(ds_transport_t *tr);

#endif /* DS_HAS_USB_HCI */

#ifdef __cplusplus
}
#endif

#endif /* DS_LINK_USB_HCI_H */

/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_bt_bluedroid.h — Bluetooth Classic transport for the original ESP32.
 *
 * Talks HID over L2CAP directly (PSM 0x11 control, 0x13 interrupt) using
 * Bluedroid's internal API, rather than esp_hidh_init(), which deadlocks on a
 * BTA semaphore under Arduino-ESP32 3.x.
 *
 * Only builds on the original ESP32: it is the one variant with a BR/EDR
 * radio. On every other target this file compiles to nothing, so a sketch that
 * only uses the protocol core still builds for the S3, C3 and friends.
 */
#ifndef DS_BT_BLUEDROID_H
#define DS_BT_BLUEDROID_H

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32) && defined(CONFIG_BT_CLASSIC_ENABLED)
#define DS_HAS_BT_BLUEDROID 1
#else
#define DS_HAS_BT_BLUEDROID 0
#endif

#include "ds_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#if DS_HAS_BT_BLUEDROID

/* Returns the singleton transport. Bluedroid only supports one HID host at a
 * time, so there is no point handing out instances.
 *
 * Set on_report / on_state / user on the returned struct before calling
 * begin(). begin() takes the controller's Bluetooth address, or NULL to go
 * looking: it runs an inquiry, takes whatever answers with a gamepad's Class
 * of Device, and keeps running one until something does. Either way an
 * already-paired pad pressing PS is let in, since that call comes to us. */
ds_transport_t *ds_bt_bluedroid(void);

/*
 * Bring the link up, naming the pad the way its address is written:
 * "AA:BB:CC:DD:EE:FF", or with dashes. NULL finds one by inquiry, which is
 * what most sketches want; name one when several are within range.
 *
 * Returns false if the string is not an address, so a typo fails here and
 * says so. Handed over as six bytes it would look like any other address and
 * the only symptom would be a host that pages nothing, for ever.
 *
 * HCI carries BD_ADDR in the opposite order to the one humans write, and both
 * reversals happen underneath: the string is parsed into the seam's wire
 * order, and the link turns it back into the order Bluedroid wants.
 */
bool ds_bt_begin(ds_transport_t *tr, const char *addr);

/* Short fixed strings about the connection: who is being paged, who called,
 * why a channel was refused. They also go to ESP_LOGI, which Arduino compiles
 * out at the default debug level -- so without this a link that will not come
 * up is completely silent. Wire it to Serial.println. */
void ds_bt_on_log(ds_transport_t *tr,
                   void (*cb)(void *user, const char *msg), void *user);

/* The pad this link is talking to, six bytes in HCI wire order to match the
 * rest of the library, or NULL if it does not know one yet. */
const uint8_t *ds_bt_peer(ds_transport_t *tr);

/* Once a pad is bonded only that pad may connect: others are refused, and no
 * inquiry runs, so a different pad in pairing mode is never picked up. This
 * forgets every bond and goes back to looking for one. Call after begin().
 * begin(addr) pins to that address instead, and this does not change it. */
void ds_bt_clear_bonds(ds_transport_t *tr);

/* Name this ESP32 advertises while emulating a console. Call before begin(). */
void ds_bt_set_device_name(const char *name);

/* Asks the controller for its calibration feature report (0x05), which is what
 * makes a DualSense switch from the 10-byte compatibility report to the full
 * 78-byte report 0x31. Normally unnecessary: sending any output report has the
 * same effect, and this transport sends one as soon as it connects. */
void ds_bt_request_full_report(void);

#endif /* DS_HAS_BT_BLUEDROID */

#ifdef __cplusplus
}
#endif

#endif /* DS_BT_BLUEDROID_H */

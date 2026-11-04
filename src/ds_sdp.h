/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_sdp.h — the smallest service discovery server a DualSense will accept.
 *
 * A pad that pages the host opens a channel to PSM 0x0001 before it settles
 * in and asks what the host is. Refusing that channel is not an answer it
 * takes: measured, it carries on for thirty seconds, gives up, and pages
 * again — which from the host's side looks like a link that dies for no
 * reason half a minute in, every time, and only ever on the path where the
 * pad did the paging.
 *
 * It does not appear to care what the answer says. BTstack holds the same
 * link for as long as it is asked to, and it calls sdp_init() and registers
 * no records whatsoever, so every query it receives comes back "nothing
 * here". A well-formed nothing is the whole requirement, and that is what
 * this returns.
 *
 * Everything in SDP is big endian, unlike the rest of Bluetooth.
 */
#ifndef DS_SDP_H
#define DS_SDP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest response built here: five bytes of header, four of counts, an
 * empty sequence and a continuation byte. */
#define DS_SDP_RSP_MAX 16

/*
 * Answers one request. Returns the number of bytes written to rsp, or zero
 * if the request was too short to carry a transaction id — in which case
 * there is nothing to answer and nowhere to send it.
 */
uint16_t ds_sdp_respond(const uint8_t *req, uint16_t len, uint8_t *rsp,
                         uint16_t rsplen);

#ifdef __cplusplus
}
#endif

#endif /* DS_SDP_H */

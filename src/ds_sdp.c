/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_sdp.c — see ds_sdp.h.
 */
#include "ds_sdp.h"

#include <stddef.h>

/* Protocol Data Unit ids. Only the three requests are ever seen here; a pad
 * asks and never answers. */
#define PDU_ERROR_RSP          0x01
#define PDU_SERVICE_SEARCH_REQ 0x02
#define PDU_SERVICE_SEARCH_RSP 0x03
#define PDU_SERVICE_ATTR_REQ   0x04
#define PDU_SERVICE_ATTR_RSP   0x05
#define PDU_SEARCH_ATTR_REQ    0x06
#define PDU_SEARCH_ATTR_RSP    0x07

/* The only one worth returning: whatever was asked, this end cannot parse it
 * well enough to say more, and saying so is better than silence. */
#define ERR_INVALID_SYNTAX     0x0003

/* pdu id, transaction id, parameter length */
#define HDR_LEN 5

/*
 * A data element sequence with nothing in it: type 6 in the top five bits,
 * size index 5 in the bottom three (meaning one length byte follows), then a
 * length of zero. This is how "no attributes" is spelled.
 */
#define DE_SEQ_EMPTY_TYPE 0x35
#define DE_SEQ_EMPTY_LEN  0x00

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t header(uint8_t *rsp, uint8_t pdu, uint16_t tid, uint16_t plen) {
  rsp[0] = pdu;
  put16(rsp + 1, tid);
  put16(rsp + 3, plen);
  return HDR_LEN;
}

uint16_t ds_sdp_respond(const uint8_t *req, uint16_t len, uint8_t *rsp,
                         uint16_t rsplen) {
  uint16_t tid;
  uint16_t at;

  if (req == NULL || rsp == NULL) return 0;
  if (len < HDR_LEN || rsplen < DS_SDP_RSP_MAX) return 0;

  /* The transaction id is echoed back untouched; it is how the far end pairs
   * an answer with its question. */
  tid = (uint16_t)(((uint16_t)req[1] << 8) | req[2]);

  switch (req[0]) {
    case PDU_SERVICE_SEARCH_REQ:
      /* Total record count, current record count, a handle list of no
       * handles, and no continuation. */
      at = header(rsp, PDU_SERVICE_SEARCH_RSP, tid, 5);
      put16(rsp + at, 0);
      at = (uint16_t)(at + 2);
      put16(rsp + at, 0);
      at = (uint16_t)(at + 2);
      rsp[at++] = 0;
      return at;

    case PDU_SERVICE_ATTR_REQ:
    case PDU_SEARCH_ATTR_REQ:
      /* Byte count, one empty sequence, and no continuation. The two differ
       * only in which response id they carry. */
      at = header(rsp,
                  req[0] == PDU_SERVICE_ATTR_REQ ? PDU_SERVICE_ATTR_RSP
                                                 : PDU_SEARCH_ATTR_RSP,
                  tid, 5);
      put16(rsp + at, 2);
      at = (uint16_t)(at + 2);
      rsp[at++] = DE_SEQ_EMPTY_TYPE;
      rsp[at++] = DE_SEQ_EMPTY_LEN;
      rsp[at++] = 0;
      return at;

    default:
      at = header(rsp, PDU_ERROR_RSP, tid, 2);
      put16(rsp + at, ERR_INVALID_SYNTAX);
      at = (uint16_t)(at + 2);
      return at;
  }
}

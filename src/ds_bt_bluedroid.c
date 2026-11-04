/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
#include "ds_bt_bluedroid.h"

#include "ds_hci.h"        /* the address parser */
#include "ds_hci_host.h"   /* and the gamepad Class of Device filter */

#if DS_HAS_BT_BLUEDROID

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Arduino frees ~36 KB of Bluetooth controller memory before setup() unless a
 * linked library says it needs it, which makes esp_bt_controller_init() fail
 * with ESP_ERR_NO_MEM. The core signals this with a constructor in
 * esp32-hal-alloc-bt-classic-mem.h, but that only works from C++; a strong
 * override of the weak hook does the same job from C and is documented in
 * esp32-hal-bt.h ("Users may also provide their own strong *InUse()"). */
#include "esp32-hal-bt.h"

bool btClassicInUse(void) { return true; }

#include "osi/allocator.h"
#include "stack/btm_api.h"
#include "stack/l2c_api.h"

#define TAG "ds5-bt"

/* No authentication or encryption; a DualSense in pairing mode expects none. */
#define BTM_SEC_NONE 0x0000

/* The pad does not accept an L2CAP connection the instant it powers up, so the
 * first attempt normally fails and the retry lands. */
#define RETRY_INTERVAL_US 5000000ULL

/* ========================================================================== */
/*                                  S T A T E                                 */
/* ========================================================================== */

static ds_transport_t s_tr;

/* The factory motion calibration, and the bytes it arrived as. Both are kept
 * because the report reaches us before anything has named the pad, and the
 * two controllers order the gyro fields differently. */
static ds_calibration_t s_cal;
static uint8_t s_cal_raw[DS5_FEATURE_CALIBRATION_SIZE];
static uint16_t s_cal_raw_len;

static uint16_t s_ctrl_cid, s_intr_cid;
static bool s_ctrl_ready, s_intr_ready;

/* In Bluedroid's order, which is the order an address is written. */
static uint8_t s_bd_addr[6];
static bool s_peer_known;             /* s_bd_addr names somebody */
static bool s_has_addr, s_l2cap_up;

/*
 * Who may call in, in Bluedroid's order. s_bd_addr cannot serve: it is
 * overwritten by whoever connects. begin(addr) pins to that pad; otherwise
 * the pads Bluedroid holds a bond for do, and only with no bond at all is
 * anyone let in — that is first pairing. The bond list is cached because it
 * is asked for from inside L2CAP callbacks.
 */
#define MAX_PINNED_BONDS 8
static uint8_t s_target[6];
static bool s_has_target;
static uint8_t s_bonds[MAX_PINNED_BONDS][6];
static int s_bond_n;
static bool s_seek_mode;              /* begin() named nobody: go looking */
static bool s_incoming;               /* the pad started this connection */
static ds_conn_state_t s_state = DS_DISCONNECTED;
static esp_timer_handle_t s_retry;
static const char *s_device_name = "ESP32_DualSense";

/* Where the HID report sits inside a received L2CAP buffer. Bluedroid puts the
 * payload at data + offset, but the previous generation of this code hardcoded
 * 9, so the value is validated rather than assumed and then latched. */
static int s_rx_base = -1;

static void (*s_on_log)(void *user, const char *msg);
static void *s_log_user;

/* Everything this link had to say went to ESP_LOGI, which Arduino compiles
 * out at the default debug level -- so a link that would not come up said
 * nothing whatsoever. Both, now: the log for a debug build, the callback for
 * a sketch that asked. */
static void say(const char *fmt, ...) {
  char line[96];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  ESP_LOGI(TAG, "%s", line);
  if (s_on_log != NULL) s_on_log(s_log_user, line);
}

static const char *addr_str(const uint8_t a[6]) {
  static char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           a[0], a[1], a[2], a[3], a[4], a[5]);
  return buf;
}

static void set_state(ds_conn_state_t st) {
  if (s_state == st) return;
  s_state = st;
  /* The pad that just left named itself; the next one has not. Clearing this
   * here rather than at each call site is what keeps a DualSense from being
   * reported for the seconds after a DualShock 4 unpairs. */
  if (st == DS_DISCONNECTED) ds_transport_forget_device(&s_tr);
  if (s_tr.on_state) s_tr.on_state(st, s_tr.user);
}

/* ========================================================================== */
/*                              P I N N I N G                                 */
/* ========================================================================== */

static void bonds_refresh(void) {
  esp_bd_addr_t list[MAX_PINNED_BONDS];
  int n = esp_bt_gap_get_bond_device_num();
  int i;

  if (n > MAX_PINNED_BONDS) {
    say("bt: %d bonds, only the first %d are let in", n, MAX_PINNED_BONDS);
    n = MAX_PINNED_BONDS;
  }
  if (n <= 0 || esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) n = 0;
  for (i = 0; i < n; i++) memcpy(s_bonds[i], list[i], 6);
  s_bond_n = n;
}

static bool pinned(void) {
  return s_has_target || s_bond_n > 0;
}

static bool peer_allowed(const uint8_t bda[6]) {
  int i;
  if (s_has_target) return memcmp(bda, s_target, 6) == 0;
  if (s_bond_n == 0) return true;
  for (i = 0; i < s_bond_n; i++) {
    if (memcmp(bda, s_bonds[i], 6) == 0) return true;
  }
  return false;
}

/* ========================================================================== */
/*                            R E T R Y   T I M E R                           */
/* ========================================================================== */

static void retry_cb(void *arg);

static void retry_start(void) {
  if (!s_retry) {
    const esp_timer_create_args_t args = {
      .callback = retry_cb, .arg = NULL, .name = "ds_retry",
    };
    esp_timer_create(&args, &s_retry);
  }
  esp_timer_stop(s_retry);
  esp_timer_start_once(s_retry, RETRY_INTERVAL_US);
}

static void retry_stop(void) {
  if (s_retry) esp_timer_stop(s_retry);
}

static void retry_cb(void *arg) {
  (void)arg;
  if (!s_l2cap_up || s_ctrl_cid) return;
  s_ctrl_cid = L2CA_ErtmConnectReq(BT_PSM_HID_CONTROL, s_bd_addr, NULL);
  say("bt: retry %s -> cid=%u", addr_str(s_bd_addr), s_ctrl_cid);
  if (!s_ctrl_cid) retry_start();
}

/* ========================================================================== */
/*                                 S E E K I N G                              */
/* ========================================================================== */

/*
 * Inquiry, in units of 1.28 s. Twelve seconds is long enough to catch a pad
 * that was put into pairing mode a moment after the board came up, and short
 * enough that a fresh one starts before anybody wonders.
 */
#define INQUIRY_LEN 10

static bool busy(void) {
  return s_ctrl_cid != 0 || s_intr_cid != 0 || s_state == DS_CONNECTED;
}

static void seek_start(void) {
  /* A pinned host has nothing to look for: its pad calls in when PS is
   * pressed, and an inquiry would only find some other pad in pairing mode. */
  if (!s_seek_mode || busy() || pinned()) return;
  esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                             INQUIRY_LEN, 0);
  if (err != ESP_OK) say("bt: inquiry would not start (%s)",
                         esp_err_to_name(err));
}

/* ========================================================================== */
/*                                   G A P                                    */
/* ========================================================================== */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      /* say(), not ESP_LOG: a key the pad rejects is the usual reason PS
       * stops working, and Arduino compiles ESP_LOG out. */
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        say("bt: authenticated %s", addr_str(param->auth_cmpl.bda));
        /* A first pairing has just made a bond; from here on only it may
         * call in. */
        bonds_refresh();
      } else
        say("bt: authentication with %s failed, status=%d",
            addr_str(param->auth_cmpl.bda), param->auth_cmpl.stat);
      break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
      /* pin_code is declared as uint8_t[16]; a string literal would be read
       * past its end. */
      esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
      break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;

    case ESP_BT_GAP_DISC_RES_EVT: {
      /*
       * Class of Device is a hint rather than proof, but it is the same hint
       * the dongle route runs on and no other peripheral in the room claims
       * to be a gamepad. Anything else discoverable is ignored in silence.
       */
      uint32_t cod = 0;
      int i;
      for (i = 0; i < param->disc_res.num_prop; i++) {
        if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_COD)
          cod = *(uint32_t *)param->disc_res.prop[i].val;
      }
      if (busy() || !ds_hci_cod_is_gamepad(cod)) break;

      memcpy(s_bd_addr, param->disc_res.bda, BD_ADDR_LEN);
      s_peer_known = true;
      s_incoming = false;
      esp_bt_gap_cancel_discovery();

      s_ctrl_cid = L2CA_ErtmConnectReq(BT_PSM_HID_CONTROL, s_bd_addr, NULL);
      say("bt: found %s -> cid=%u", addr_str(s_bd_addr), s_ctrl_cid);
      if (!s_ctrl_cid) retry_start();
      break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      /* Inquiry runs for a fixed time and then stops itself. Nothing found
       * and nothing connected means go round again. */
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED)
        seek_start();
      break;

    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
      /* The first sign of a PS press: everything after it is this end's
       * doing, everything before it the pad's. */
      say("bt: link from %s up, status=%d",
          addr_str(param->acl_conn_cmpl_stat.bda),
          param->acl_conn_cmpl_stat.stat);
      break;

    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
      say("bt: link to %s dropped, reason=0x%02X",
          addr_str(param->acl_disconn_cmpl_stat.bda),
          param->acl_disconn_cmpl_stat.reason);
      break;

    default:
      break;
  }
}

/* ========================================================================== */
/*                      I N T E R R U P T   C H A N N E L                     */
/* ========================================================================== */

static void ready_check(void) {
  if (s_ctrl_ready && s_intr_ready) {
    s_ctrl_ready = s_intr_ready = false;

    /* Both pads answer with report 0x01 until something asks them for a
     * feature report, and 0x01 is sticks, buttons and triggers only — no
     * motion, no touchpad, no battery. It is also the one report *both*
     * controllers send, so while it is all that arrives there is nothing to
     * tell a DualShock 4 from a DualSense and the link keeps calling it a
     * DualSense.
     *
     * The request below is what switches either pad to its full report: 0x31
     * on a DualSense, 0x11 on a DualShock 4. It has to go out before the
     * state change, because a sketch may look at device() the moment it is
     * told it is connected. */
    ds_bt_request_full_report();

    set_state(DS_CONNECTED);
  }
}

static void intr_connect_ind(BD_ADDR bda, uint16_t lcid, uint16_t psm, uint8_t id) {
  (void)psm;
  if (!peer_allowed(bda)) {
    L2CA_ErtmConnectRsp(bda, id, lcid, L2CAP_CONN_SECURITY_BLOCK, 0, NULL);
    return;
  }
  L2CA_ErtmConnectRsp(bda, id, lcid, L2CAP_CONN_OK, 0, NULL);
  tL2CAP_CFG_INFO cfg = {0};
  L2CA_ConfigReq(lcid, &cfg);
  s_intr_cid = lcid;
  memcpy(s_bd_addr, bda, BD_ADDR_LEN);
  s_peer_known = true;
}

static void intr_connect_cfm(uint16_t lcid, uint16_t result) {
  if (result == L2CAP_CONN_OK) {
    s_intr_cid = lcid;
    tL2CAP_CFG_INFO cfg = {0};
    L2CA_ConfigReq(lcid, &cfg);
  } else {
    ESP_LOGW(TAG, "INTR connect failed, result=%u", result);
    s_intr_cid = 0;
  }
}

static void intr_config_ind(uint16_t lcid, tL2CAP_CFG_INFO *cfg) {
  cfg->result = L2CAP_CFG_OK;
  L2CA_ConfigRsp(lcid, cfg);
}

static void intr_config_cfm(uint16_t lcid, tL2CAP_CFG_INFO *cfg) {
  (void)lcid;
  if (cfg->result == L2CAP_CFG_OK) {
    s_intr_ready = true;
    ready_check();
  }
}

static void intr_disconnect_ind(uint16_t lcid, bool ack_needed) {
  if (ack_needed) L2CA_DisconnectRsp(lcid);
  if (lcid == s_intr_cid) {
    s_intr_cid = 0;
    s_intr_ready = s_ctrl_ready = false;
    set_state(DS_DISCONNECTED);
    seek_start();
  }
}

static void intr_disconnect_cfm(uint16_t lcid, uint16_t result) {
  (void)result;
  if (lcid == s_intr_cid) { s_intr_cid = 0; s_intr_ready = false; }
}

/* Finds the HID report inside the L2CAP payload and hands it up. */
static void intr_data_ind(uint16_t lcid, BT_HDR *p_buf) {
  (void)lcid;
  if (!p_buf) return;

  const uint8_t *d = p_buf->data;
  uint16_t end = (uint16_t)(p_buf->offset + p_buf->length);
  int cand[3];
  int n = 0;
  if (s_rx_base >= 0) cand[n++] = s_rx_base;
  cand[n++] = p_buf->offset;
  cand[n++] = 9;   /* observed on Arduino-ESP32 3.3.x; kept as a fallback */

  for (int i = 0; i < n; i++) {
    int b = cand[i];
    if (b < 0 || b + 2 > end) continue;

    uint16_t len = 0;
    const uint8_t *report = ds_find_bt_report(d + b, (uint16_t)(end - b), &len);
    if (!report) continue;

    if (s_rx_base != b) {
      ESP_LOGI(TAG, "input base=%d (offset=%u length=%u id=0x%02X)",
               b, p_buf->offset, p_buf->length, report[0]);
      s_rx_base = b;
    }
    /* No product id on this link, so the report id is what names the pad.
     * 0x01 arrives first and identifies nothing, but both pads lay it out the
     * same way, so it decodes correctly while we still do not know. */
    ds_device_t seen;
    if (ds_identify_bt_report(report[0], &seen)) {
      s_tr.device = seen;
      s_tr.device_named = true;
      /* The calibration was read before this was known, and a DualShock 4
       * orders its gyro fields the other way. Read it again now. */
      if (s_cal_raw_len > 0) {
        ds_parse_calibration_dev(s_cal_raw, s_cal_raw_len, DS_LINK_BT,
                                 seen, &s_cal);
      }
    }

    if (s_tr.on_report) s_tr.on_report(report, len, s_tr.user);
    break;
  }

  osi_free(p_buf);
}

static void intr_qos(BD_ADDR bda) { (void)bda; }
static void intr_congestion(uint16_t lcid, bool c) { (void)lcid; (void)c; }
static void intr_tx_complete(uint16_t lcid, uint16_t n) { (void)lcid; (void)n; }
static void intr_connect_pnd(uint16_t lcid) { (void)lcid; }

static tL2CAP_APPL_INFO s_intr_appl = {
  .pL2CA_ConnectInd_Cb = intr_connect_ind,
  .pL2CA_ConnectCfm_Cb = intr_connect_cfm,
  .pL2CA_ConnectPnd_Cb = intr_connect_pnd,
  .pL2CA_ConfigInd_Cb = intr_config_ind,
  .pL2CA_ConfigCfm_Cb = intr_config_cfm,
  .pL2CA_DisconnectInd_Cb = intr_disconnect_ind,
  .pL2CA_DisconnectCfm_Cb = intr_disconnect_cfm,
  .pL2CA_QoSViolationInd_Cb = intr_qos,
  .pL2CA_DataInd_Cb = intr_data_ind,
  .pL2CA_CongestionStatus_Cb = intr_congestion,
  .pL2CA_TxComplete_Cb = intr_tx_complete,
};

/* ========================================================================== */
/*                        C O N T R O L   C H A N N E L                       */
/* ========================================================================== */

static void ctrl_connect_ind(BD_ADDR bda, uint16_t lcid, uint16_t psm, uint8_t id) {
  (void)psm;
  /* Checked before anything below, which would drop our own attempt and
   * overwrite s_bd_addr for a caller we are about to refuse. */
  if (!peer_allowed(bda)) {
    say("bt: %s is not the bonded pad, refusing", addr_str(bda));
    L2CA_ErtmConnectRsp(bda, id, lcid, L2CAP_CONN_SECURITY_BLOCK, 0, NULL);
    return;
  }

  /* The pad is calling us (PS button reconnect). Abandon any outgoing attempt
   * so the two do not fight over the channel. */
  retry_stop();
  if (s_ctrl_cid && s_ctrl_cid != lcid) L2CA_DisconnectReq(s_ctrl_cid);

  s_incoming = true;
  s_ctrl_ready = s_intr_ready = false;
  esp_bt_gap_cancel_discovery();
  memcpy(s_bd_addr, bda, BD_ADDR_LEN);
  s_peer_known = true;
  say("bt: %s is calling us", addr_str(s_bd_addr));

  L2CA_ErtmConnectRsp(bda, id, lcid, L2CAP_CONN_OK, 0, NULL);
  tL2CAP_CFG_INFO cfg = {0};
  L2CA_ConfigReq(lcid, &cfg);
  s_ctrl_cid = lcid;
}

static void ctrl_connect_cfm(uint16_t lcid, uint16_t result) {
  if (result == L2CAP_CONN_OK) {
    retry_stop();
    s_ctrl_cid = lcid;
    tL2CAP_CFG_INFO cfg = {0};
    L2CA_ConfigReq(lcid, &cfg);
  } else {
    say("bt: ctrl refused (result=%u), retrying in 5s", result);
    s_ctrl_cid = 0;
    retry_start();
  }
}

static void ctrl_config_ind(uint16_t lcid, tL2CAP_CFG_INFO *cfg) {
  cfg->result = L2CAP_CFG_OK;
  L2CA_ConfigRsp(lcid, cfg);
}

static void ctrl_config_cfm(uint16_t lcid, tL2CAP_CFG_INFO *cfg) {
  (void)lcid;
  if (cfg->result != L2CAP_CFG_OK) return;
  s_ctrl_ready = true;

  /* On an outgoing connection we open the interrupt channel ourselves. On an
   * incoming one the pad opens it, and doing both gets us two channels. */
  if (!s_incoming && !s_intr_cid && s_peer_known) {
    s_intr_cid = L2CA_ErtmConnectReq(BT_PSM_HID_INTERRUPT, s_bd_addr, NULL);
  }
  ready_check();
}

static void ctrl_disconnect_ind(uint16_t lcid, bool ack_needed) {
  if (ack_needed) L2CA_DisconnectRsp(lcid);
  if (lcid == s_ctrl_cid) { s_ctrl_cid = 0; s_ctrl_ready = false; }
}

static void ctrl_disconnect_cfm(uint16_t lcid, uint16_t result) {
  (void)result;
  if (lcid == s_ctrl_cid) { s_ctrl_cid = 0; s_ctrl_ready = false; }
}

/* The control channel carries exactly one thing this library asks for: the
 * calibration, requested as the pad is switched to full reports. A report that
 * fails its CRC is not used — scaling every later reading by something corrupt
 * is worse than leaving the motion raw. */
static void ctrl_data_ind(uint16_t lcid, BT_HDR *p_buf) {
  (void)lcid;
  if (p_buf == NULL) return;

  /* Same probing as intr_data_ind: where the payload starts inside the
   * buffer is not consistent across Arduino-ESP32 builds, so try the known
   * candidates rather than trusting one. */
  const uint8_t *d = p_buf->data;
  uint16_t end = (uint16_t)(p_buf->offset + p_buf->length);
  int cand[2];
  int n = 0;
  cand[n++] = p_buf->offset;
  cand[n++] = 9;

  for (int i = 0; i < n; i++) {
    int b = cand[i];
    if (b < 0 || b + 2 > end) continue;

    uint16_t len = 0;
    const uint8_t *report = ds_find_bt_feature(d + b, (uint16_t)(end - b), &len);
    /* Over Bluetooth both pads keep the calibration behind 0x05, at the same
     * length. Only the field order inside differs. */
    if (report == NULL || report[0] != DS5_FEATURE_CALIBRATION) continue;
    if (len > sizeof(s_cal_raw)) continue;

    memcpy(s_cal_raw, report, len);
    s_cal_raw_len = len;
    /* Read with what is known now, and again below once the pad names itself. */
    ds_parse_calibration_dev(report, len, DS_LINK_BT, s_tr.device, &s_cal);
    break;
  }
  osi_free(p_buf);
}

static const ds_calibration_t *tr_calibration(ds_transport_t *tr) {
  (void)tr;
  return s_cal.valid ? &s_cal : NULL;
}

static void ctrl_qos(BD_ADDR bda) { (void)bda; }
static void ctrl_congestion(uint16_t lcid, bool c) { (void)lcid; (void)c; }
static void ctrl_tx_complete(uint16_t lcid, uint16_t n) { (void)lcid; (void)n; }
static void ctrl_connect_pnd(uint16_t lcid) { (void)lcid; }

static tL2CAP_APPL_INFO s_ctrl_appl = {
  .pL2CA_ConnectInd_Cb = ctrl_connect_ind,
  .pL2CA_ConnectCfm_Cb = ctrl_connect_cfm,
  .pL2CA_ConnectPnd_Cb = ctrl_connect_pnd,
  .pL2CA_ConfigInd_Cb = ctrl_config_ind,
  .pL2CA_ConfigCfm_Cb = ctrl_config_cfm,
  .pL2CA_DisconnectInd_Cb = ctrl_disconnect_ind,
  .pL2CA_DisconnectCfm_Cb = ctrl_disconnect_cfm,
  .pL2CA_QoSViolationInd_Cb = ctrl_qos,
  .pL2CA_DataInd_Cb = ctrl_data_ind,
  .pL2CA_CongestionStatus_Cb = ctrl_congestion,
  .pL2CA_TxComplete_Cb = ctrl_tx_complete,
};

/* ========================================================================== */
/*                              B R I N G - U P                               */
/* ========================================================================== */

static void stack_up(void) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
  esp_bt_gap_set_device_name(s_device_name);
#else
  esp_bt_dev_set_device_name(s_device_name);
#endif
  esp_bt_gap_register_callback(gap_cb);

  /* Class of Device "computer / desktop", so the pad treats us as a host. */
  esp_bt_cod_t cod = {0};
  cod.major = 0x01;
  cod.minor = 0x01;
  esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);

  uint8_t iocap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

  /* Claim four free security records. BTM_SEC_SERVICE_FIRST_EMPTY differs
   * between IDF v4 (51) and v5 (54); see stack/btm_api.h. */
  BTM_SetSecurityLevel(false, "HID_CTRL", BTM_SEC_SERVICE_FIRST_EMPTY + 0,
                       BTM_SEC_NONE, BT_PSM_HID_CONTROL, 0, 0);
  BTM_SetSecurityLevel(true, "HID_CTRL", BTM_SEC_SERVICE_FIRST_EMPTY + 1,
                       BTM_SEC_NONE, BT_PSM_HID_CONTROL, 0, 0);
  BTM_SetSecurityLevel(false, "HID_INTR", BTM_SEC_SERVICE_FIRST_EMPTY + 2,
                       BTM_SEC_NONE, BT_PSM_HID_INTERRUPT, 0, 0);
  BTM_SetSecurityLevel(true, "HID_INTR", BTM_SEC_SERVICE_FIRST_EMPTY + 3,
                       BTM_SEC_NONE, BT_PSM_HID_INTERRUPT, 0, 0);

  uint16_t a = L2CA_Register(BT_PSM_HID_CONTROL, &s_ctrl_appl);
  uint16_t b = L2CA_Register(BT_PSM_HID_INTERRUPT, &s_intr_appl);
  if (!a || !b) say("bt: PSM registration failed (ctrl=%u intr=%u)", a, b);

  s_l2cap_up = true;
  bonds_refresh();

  if (s_has_addr) {
    s_has_addr = false;
    s_incoming = false;
    s_ctrl_cid = L2CA_ErtmConnectReq(BT_PSM_HID_CONTROL, s_bd_addr, NULL);
    say("bt: connecting to %s -> cid=%u", addr_str(s_bd_addr), s_ctrl_cid);
    if (!s_ctrl_cid) retry_start();
  } else if (pinned()) {
    say("bt: bonded to %s, press PS to connect", addr_str(s_bonds[0]));
  } else {
    say("bt: looking for a pad in pairing mode");
    seek_start();
  }
}

/* ========================================================================== */
/*                        T R A N S P O R T   V - T A B L E                   */
/* ========================================================================== */

static bool tr_begin(ds_transport_t *self, const uint8_t addr[6]) {
  (void)self;

  if (addr) {
    /*
     * The seam carries a BD_ADDR in HCI wire order, which is the reverse of
     * the way one is written: "AA:BB:CC:DD:EE:FF" arrives here as
     * FF EE DD CC BB AA. Bluedroid is the other way round -- its BD_ADDR is
     * in written order and its own HCI layer reverses it on the way out --
     * so the bytes have to be turned back before it sees them.
     *
     * Handing them over as they arrive pages an address nobody has, and the
     * only symptom is a host that finds nothing, for ever.
     */
    int i;
    for (i = 0; i < 6; i++) s_bd_addr[i] = addr[5 - i];
    memcpy(s_target, s_bd_addr, 6);
    s_has_target = true;
    s_peer_known = true;
    s_has_addr = true;
  } else {
    s_has_target = false;
  }
  s_seek_mode = (addr == NULL);

  /* btStartMode() handles controller init + enable with the right config for
   * this core build; doing it by hand tends to disagree with sdkconfig. */
  if (!btStarted() && !btStartMode(BT_MODE_CLASSIC_BT)) {
    ESP_LOGE(TAG, "btStartMode(CLASSIC) failed - is BT memory released?");
    return false;
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    esp_err_t err = esp_bluedroid_init();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(err));
      return false;
    }
  }
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    esp_err_t err = esp_bluedroid_enable();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(err));
      return false;
    }
  }

  /* Bluedroid is usable the moment enable() returns; IDF v5 removed the
   * STACK_UP callback the original code waited on. */
  set_state(DS_CONNECTING);
  stack_up();
  return true;
}

static void tr_end(ds_transport_t *self) {
  (void)self;
  retry_stop();
  if (s_retry) { esp_timer_delete(s_retry); s_retry = NULL; }
  if (s_l2cap_up) {
    L2CA_Deregister(BT_PSM_HID_CONTROL);
    L2CA_Deregister(BT_PSM_HID_INTERRUPT);
    s_l2cap_up = false;
  }
  set_state(DS_DISCONNECTED);
}

static bool tr_send(ds_transport_t *self, const uint8_t *report, uint16_t len) {
  (void)self;
  if (!s_intr_cid || !report || len < 5) return false;

  BT_HDR *p = (BT_HDR *)osi_malloc(BT_DEFAULT_BUFFER_SIZE);
  if (!p) return false;

  p->offset = L2CAP_MIN_OFFSET;
  p->length = len;
  p->event = 0;
  p->layer_specific = 0;

  /* The core already added the 0xA2 transaction byte and the CRC32, so this
   * goes out verbatim. */
  memcpy((uint8_t *)(p + 1) + p->offset, report, len);

  L2CA_DataWrite(s_intr_cid, p);
  return true;
}

static ds_conn_state_t tr_state(ds_transport_t *self) {
  (void)self;
  return s_state;
}

const uint8_t *ds_bt_peer(ds_transport_t *tr) {
  /* Bluedroid holds addresses the way they are written; the seam carries
   * them the way HCI does. Reversed into a scratch buffer rather than kept
   * twice, since nothing here is re-entrant. */
  static uint8_t wire[6];
  int i;

  (void)tr;
  if (!s_peer_known) return NULL;
  for (i = 0; i < 6; i++) wire[i] = s_bd_addr[5 - i];
  return wire;
}

ds_transport_t *ds_bt_bluedroid(void) {
  s_tr.link = DS_LINK_BT;
  s_tr.begin = tr_begin;
  s_tr.end = tr_end;
  s_tr.poll = NULL;   /* Bluedroid runs on its own task */
  s_tr.send = tr_send;
  s_tr.state = tr_state;
  s_tr.peer = ds_bt_peer;
  s_tr.calibration = tr_calibration;
  return &s_tr;
}

bool ds_bt_begin(ds_transport_t *tr, const char *addr) {
  uint8_t bd[DS_HCI_BD_ADDR_LEN];

  if (tr == NULL || tr->begin == NULL) return false;
  if (addr == NULL) return tr->begin(tr, NULL);
  if (!ds_hci_str_to_addr(addr, bd)) return false;
  return tr->begin(tr, bd);
}

void ds_bt_on_log(ds_transport_t *tr,
                   void (*cb)(void *user, const char *msg), void *user) {
  (void)tr;
  s_on_log = cb;
  s_log_user = user;
}

void ds_bt_clear_bonds(ds_transport_t *tr) {
  int i;
  (void)tr;
  if (!s_l2cap_up) return;
  bonds_refresh();
  for (i = 0; i < s_bond_n; i++) esp_bt_gap_remove_bond_device(s_bonds[i]);
  s_bond_n = 0;
  say("bt: bonds cleared, looking for a pad in pairing mode");
  seek_start();
}

void ds_bt_set_device_name(const char *name) {
  if (name) s_device_name = name;
}

void ds_bt_request_full_report(void) {
  if (!s_ctrl_cid) return;

  BT_HDR *p = (BT_HDR *)osi_malloc(BT_DEFAULT_BUFFER_SIZE);
  if (!p) return;

  p->offset = L2CAP_MIN_OFFSET;
  p->length = 2;
  p->event = 0;
  p->layer_specific = 0;

  uint8_t *d = (uint8_t *)(p + 1) + p->offset;
  d[0] = 0x43;   /* GET_REPORT | FEATURE */
  d[1] = 0x05;   /* calibration */

  L2CA_DataWrite(s_ctrl_cid, p);
}

#endif /* DS_HAS_BT_BLUEDROID */

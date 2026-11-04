/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_link_usb_hci.c — the dongle route behind the transport seam.
 * See ds_link_usb_hci.h.
 */
#include "ds_link_usb_hci.h"

#ifdef DS_HAS_USB_HCI

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>

#include "ds_sdp.h"
#include "nvs.h"

/* How long to let a pad that paged us open its own channels before doing it
 * from this end. Measured answer: it never does, so this is only a guard
 * against a future revision that behaves differently -- and every PS
 * reconnect pays it in full. Short, not zero, so a revision that does open
 * its own channels still gets a fair chance first. */
#define OPEN_GRACE_MS 300

/* A DualSense sends a report every few milliseconds, so half a second of
 * nothing is already an anomaly and not a slow moment. */
#define QUIET_MS 500

static struct {
  ds_transport_t tr;
  ds_hci_host_t host;
  ds_l2cap_t l2;
  ds_calibration_t cal;
  /* The calibration as it arrived, kept because it is read before the pad has
     said which pad it is. See the control-channel handler. */
  uint8_t cal_raw[DS5_FEATURE_CALIBRATION_SIZE];
  uint16_t cal_raw_len;

  bool started;
  bool have_bond;
  uint8_t bond_addr[DS_HCI_BD_ADDR_LEN];
  uint8_t bond_key[DS_HCI_LINK_KEY_LEN];

  bool have_target;                          /* begin() named an address */
  uint8_t target[DS_HCI_BD_ADDR_LEN];
  /* One page at start-up even when bonded, for a pad that is already on and
   * waiting. Cleared by the first attempt, set again on each attach. */
  bool first_page;

  bool connecting;                           /* a link attempt is in flight */
  uint32_t link_up_at;                       /* 0 when no encrypted link   */

  /* Enough to tell a link that went quiet from one the radio dropped, and
   * either of those from a sketch that stopped pumping USB. All three look
   * identical from the outside: "it disconnected". */
  /* True between the interrupt channel opening and closing. Deliberately not
   * link_up_at, which is the grace timer for opening the HID channels and is
   * cleared the moment they open. */
  bool hid_up;
  uint32_t last_acl_ms;                      /* pad's last word            */
  bool reported_quiet;                       /* one line per silence       */
  uint8_t seq;

  void (*on_bond)(void *user, const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                  const uint8_t key[DS_HCI_LINK_KEY_LEN]);
  void *bond_user;

  void (*on_log)(void *user, const char *msg);
  void *log_user;

  bool persist;
  bool persist_set;    /* the application said one way or the other */

  /* Everything above is touched by the pump task and by whoever calls send(),
   * so one lock covers the lot. It is held for a whole pump iteration, which
   * is short: the USB layer never blocks. */
  SemaphoreHandle_t lock;
  TaskHandle_t task;
  volatile bool stop;
} g;

/* One millisecond. The pad sends a report every two or three, and the
 * supervision timeout it asks for is five seconds, so this is not a number
 * that needs tuning — it needs only to never be missed. */
#define PUMP_PERIOD_TICKS 1

#define PUMP_STACK 4096
#define PUMP_PRIO  5

/* Defined further down, next to the work it repeats. */
static void pump_task(void *arg);

static void take(void) {
  if (g.lock != NULL) xSemaphoreTake(g.lock, portMAX_DELAY);
}

static void give(void) {
  if (g.lock != NULL) xSemaphoreGive(g.lock);
}

static void relay_log(void *ctx, const char *msg);

/* ------------------------------------------------------------------ bond --
 *
 * A link key is worth exactly one thing: not having to put the pad back into
 * pairing mode. Losing it is not an error, so none of this reports failure —
 * the worst case is one more pairing. */

#define BOND_NS "ds5bond"

/* First and last byte, which is plenty to tell one key from another and
 * does not put the whole thing in a log. */
static void say_key(const char *what) {
  static char line[48];
  snprintf(line, sizeof(line), "%s %02X..%02X", what, g.bond_key[0],
           g.bond_key[DS_HCI_LINK_KEY_LEN - 1]);
  relay_log(NULL, line);
}

static bool bond_enabled(void) {
  return g.persist_set ? g.persist : true;   /* on unless told otherwise */
}

static void bond_load(void) {
  nvs_handle_t h;
  size_t n;

  if (!bond_enabled() || g.have_bond) return;
  if (nvs_open(BOND_NS, NVS_READONLY, &h) != ESP_OK) return;

  n = DS_HCI_BD_ADDR_LEN;
  if (nvs_get_blob(h, "addr", g.bond_addr, &n) == ESP_OK &&
      n == DS_HCI_BD_ADDR_LEN) {
    n = DS_HCI_LINK_KEY_LEN;
    if (nvs_get_blob(h, "key", g.bond_key, &n) == ESP_OK &&
        n == DS_HCI_LINK_KEY_LEN) {
      g.have_bond = true;
      say_key("bond: loaded");
    }
  }
  nvs_close(h);
}

static void bond_store(void) {
  nvs_handle_t h;
  if (!bond_enabled()) return;
  if (nvs_open(BOND_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "addr", g.bond_addr, DS_HCI_BD_ADDR_LEN);
  nvs_set_blob(h, "key", g.bond_key, DS_HCI_LINK_KEY_LEN);
  nvs_commit(h);
  nvs_close(h);
  say_key("bond: saved");
}

static void bond_erase(void) {
  nvs_handle_t h;
  if (nvs_open(BOND_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

static void relay_log(void *ctx, const char *msg) {
  (void)ctx;
  if (g.on_log != NULL) g.on_log(g.log_user, msg);
}

static uint32_t now_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void announce(ds_conn_state_t st) {
  /* See the note in ds_bt_bluedroid.c: an identity outlives the pad it came
   * from unless something drops it here. */
  if (st == DS_DISCONNECTED) ds_transport_forget_device(&g.tr);
  if (g.tr.on_state != NULL) g.tr.on_state(st, g.tr.user);
}

/* ========================================================================== */
/*                              usb -> hci                                    */
/* ========================================================================== */

static void on_usb_event(void *ctx, const uint8_t *p, uint16_t n) {
  (void)ctx;
  ds_hci_host_handle_event(&g.host, p, n);
}

static void on_usb_acl(void *ctx, const uint8_t *p, uint16_t n) {
  (void)ctx;
  g.last_acl_ms = now_ms();
  g.reported_quiet = false;
  ds_hci_host_handle_acl(&g.host, p, n);
}

static void on_usb_attach(void *ctx, uint16_t vid, uint16_t pid) {
  (void)ctx; (void)vid; (void)pid;
  g.first_page = true;
  ds_hci_host_start(&g.host);
}

static void on_usb_detach(void *ctx) {
  (void)ctx;
  ds_l2cap_reset(&g.l2);
  g.connecting = false;
  g.link_up_at = 0;
  announce(DS_DISCONNECTED);
}

/* ========================================================================== */
/*                              hci <-> l2cap                                 */
/* ========================================================================== */

static void on_host_l2cap(void *ctx, uint16_t cid, const uint8_t *d,
                          uint16_t n) {
  (void)ctx;
  ds_l2cap_handle(&g.l2, cid, d, n);
}

static int l2cap_send(void *ctx, uint16_t cid, const uint8_t *d, uint16_t n) {
  (void)ctx;
  return ds_hci_host_send_l2cap(&g.host, cid, d, n) ? 0 : -1;
}

/* ========================================================================== */
/*                                 pairing                                    */
/* ========================================================================== */

static bool get_link_key(void *ctx, const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                         uint8_t key[DS_HCI_LINK_KEY_LEN]) {
  (void)ctx;
  if (!g.have_bond) return false;
  if (memcmp(addr, g.bond_addr, DS_HCI_BD_ADDR_LEN) != 0) return false;
  memcpy(key, g.bond_key, DS_HCI_LINK_KEY_LEN);
  say_key("bond: supplied");
  return true;
}

/* Who may call in: the pad begin() named, else the bonded pad, else anyone —
 * the last only while there is no bond, so first pairing still works. */
static bool accept_peer(void *ctx, const uint8_t addr[DS_HCI_BD_ADDR_LEN]) {
  (void)ctx;
  if (g.have_target) return memcmp(addr, g.target, DS_HCI_BD_ADDR_LEN) == 0;
  if (g.have_bond) return memcmp(addr, g.bond_addr, DS_HCI_BD_ADDR_LEN) == 0;
  return true;
}

static void new_link_key(void *ctx, const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                         const uint8_t key[DS_HCI_LINK_KEY_LEN]) {
  (void)ctx;
  memcpy(g.bond_addr, addr, DS_HCI_BD_ADDR_LEN);
  memcpy(g.bond_key, key, DS_HCI_LINK_KEY_LEN);
  g.have_bond = true;
  bond_store();
  if (g.on_bond != NULL) g.on_bond(g.bond_user, addr, key);
}

static void on_device_found(void *ctx, const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                            uint32_t cod, int8_t rssi, bool has_rssi) {
  (void)ctx; (void)rssi; (void)has_rssi;
  if (g.connecting || !ds_hci_cod_is_gamepad(cod)) return;
  g.connecting = true;
  ds_hci_host_connect(&g.host, addr);
}

/* ========================================================================== */
/*                              link progress                                 */
/* ========================================================================== */

static void on_host_state(void *ctx, ds_hci_host_state_t st) {
  (void)ctx;

  /* An ACL link that drops takes its channels with it without saying so on
   * the wire, and a channel table that still believes otherwise refuses the
   * next attempt for a PSM it thinks is in use. That includes SDP: a pad that
   * called in and lost the link before closing it would otherwise be refused
   * SDP on every later PS press, so the table is cleared whether or not HID
   * ever opened. Only the announcement depends on that. */
  if (st == DS_HOST_READY || st == DS_HOST_FAILED ||
      st == DS_HOST_INITIALISING) {
    bool had_hid = ds_l2cap_channel(&g.l2, DS_L2CAP_PSM_HID_CONTROL) != NULL;
    ds_l2cap_reset(&g.l2);
    if (had_hid) {
      memset(&g.cal, 0, sizeof(g.cal));
      announce(DS_DISCONNECTED);
    }
  }

  switch (st) {
    case DS_HOST_READY:
    case DS_HOST_FAILED:
      /* 0x05 and 0x06 both mean the two ends do not hold the same key: the
       * pad was put back into pairing mode, or paired with something else,
       * and forgot this host. Supplying the old key again cannot ever work,
       * and retrying it is what turns a one-off into "it never reconnects".
       * Dropping it here puts the next attempt back on the pairing path,
       * which is the only thing that can fix it. */
      if (st == DS_HOST_FAILED && g.have_bond &&
          (g.host.last_auth_status == 0x05 || g.host.last_auth_status == 0x06)) {
        relay_log(NULL, "bond: rejected by pad, forgetting it");
        bond_erase();
        g.have_bond = false;
        memset(g.bond_addr, 0, sizeof(g.bond_addr));
        memset(g.bond_key, 0, sizeof(g.bond_key));
        g.host.last_auth_status = 0;
      }

      g.connecting = false;
      g.link_up_at = 0;
      if (st == DS_HOST_FAILED) announce(DS_LINK_ERROR);
      if (g.have_target) {
        /*
         * Page only while there is no bond with it, which is when pairing
         * needs this end to connect, plus once at start-up. A bonded pad
         * calls in on PS by itself, and paging for it after every drop kept
         * the controller busy exactly when that call arrived -- the call was
         * dropped and the connection came down to which page won. Same as
         * begin(addr) on the original ESP32.
         */
        bool bonded = g.have_bond &&
                      memcmp(g.bond_addr, g.target, DS_HCI_BD_ADDR_LEN) == 0;
        if (!bonded || g.first_page) {
          g.first_page = false;
          g.connecting = true;
          ds_hci_host_connect(&g.host, g.target);
        }
      } else if (!g.have_bond) {
        /* Nothing to wait for: hunt for a pad in pairing mode. */
        ds_hci_host_inquiry(&g.host, 8);
      }
      /* Otherwise do nothing at all — page scan is already on and the pad
       * calls in when PS is pressed. Scanning here would stop the controller
       * answering that page. */
      break;

    case DS_HOST_CONNECTING:
      g.connecting = true;
      announce(DS_CONNECTING);
      break;

    case DS_HOST_CONNECTED:
      g.link_up_at = now_ms();
      /*
       * Only when this end paged. A pad that called in opens the channels
       * itself, and reaching for them at the same moment leaves it holding
       * one of ours that it answered and nothing here uses. link_up_at is
       * the grace timer, which opens them from this end if the pad turns out
       * not to -- so nothing is lost if a future revision behaves otherwise.
       */
      if (!g.host.inbound) ds_l2cap_connect_hid(&g.l2);
      break;

    default:
      break;
  }
}

static void on_chan_open(void *ctx, uint16_t psm) {
  (void)ctx;

  if (psm == DS_L2CAP_PSM_HID_CONTROL) {
    /* Ask for the calibration while the interrupt channel is still coming
     * up, so it is usually in before the first motion report. */
    uint8_t req[2];
    uint16_t n = ds_build_bt_feature_request(DS5_FEATURE_CALIBRATION, req,
                                              sizeof(req));
    if (n) ds_l2cap_send(&g.l2, DS_L2CAP_PSM_HID_CONTROL, req, n);
    return;
  }
  if (psm != DS_L2CAP_PSM_HID_INTERRUPT) return;
  g.link_up_at = 0;
  g.hid_up = true;
  g.last_acl_ms = now_ms();
  g.reported_quiet = false;
  announce(DS_CONNECTED);
}

static void on_chan_close(void *ctx, uint16_t psm) {
  (void)ctx;
  if (psm != DS_L2CAP_PSM_HID_INTERRUPT) return;
  /*
   * Only if it was ever open. Both ends reach for the HID channels at once on
   * a reconnect, and the half-built attempt that gives way closes like any
   * other -- but nothing above here was ever told the pad had arrived, so
   * telling them it left announces a disconnection in the middle of a
   * connection.
   */
  if (!g.hid_up) return;
  g.hid_up = false;
  memset(&g.cal, 0, sizeof(g.cal));
  announce(DS_DISCONNECTED);
}

static void on_hid_data(void *ctx, uint16_t psm, const uint8_t *d, uint16_t n) {
  const uint8_t *report;
  uint16_t len = 0;
  (void)ctx;

  if (psm == DS_L2CAP_PSM_SDP) {
    /* The pad wants to know what it has connected to. It is not fussy about
     * the answer -- an empty one is what the stacks that hold this link
     * return -- but it does insist on getting one. */
    uint8_t rsp[DS_SDP_RSP_MAX];
    uint16_t rn = ds_sdp_respond(d, n, rsp, sizeof(rsp));
    if (rn != 0) ds_l2cap_send(&g.l2, DS_L2CAP_PSM_SDP, rsp, rn);
    return;
  }

  if (psm == DS_L2CAP_PSM_HID_CONTROL) {
    /* The only thing asked for is the calibration, and a report failing its
     * CRC is simply not used: scaling every later reading by something
     * corrupt is worse than leaving the motion raw. */
    report = ds_find_bt_feature(d, n, &len);
    if (report != NULL && report[0] == DS5_FEATURE_CALIBRATION) {
      /* Both pads keep this behind 0x05 over Bluetooth, at the same length,
       * and it arrives *before* the first full report — which is the only
       * thing that says which pad this is. So keep the bytes and read them
       * again once we know: a DualShock 4 groups the gyro plus readings where
       * a DualSense pairs them per axis, and guessing scales pitch by roll's
       * numbers. */
      if (len <= sizeof(g.cal_raw)) {
        memcpy(g.cal_raw, report, len);
        g.cal_raw_len = len;
      }
      ds_parse_calibration_dev(report, len, DS_LINK_BT, g.tr.device, &g.cal);
    }
    return;
  }
  if (psm != DS_L2CAP_PSM_HID_INTERRUPT) return;

  report = ds_find_bt_report(d, n, &len);
  if (report != NULL) {
    ds_device_t seen;
    if (ds_identify_bt_report(report[0], &seen) &&
        (seen != g.tr.device || !g.tr.device_named)) {
      g.tr.device = seen;
      g.tr.device_named = true;
      /* Now that the pad has named itself, the calibration read earlier was
       * very likely read the wrong way round. Do it again. */
      if (g.cal_raw_len > 0) {
        ds_parse_calibration_dev(g.cal_raw, g.cal_raw_len, DS_LINK_BT,
                                  seen, &g.cal);
      }
    }
  }
  if (report != NULL && g.tr.on_report != NULL) {
    g.tr.on_report(report, len, g.tr.user);
  }
}

/* ========================================================================== */
/*                             transport methods                              */
/* ========================================================================== */

static bool link_begin(ds_transport_t *self,
                       const uint8_t addr[DS_HCI_BD_ADDR_LEN]) {
  ds_hci_host_config_t hcfg;
  ds_l2cap_config_t lcfg;
  ds_usb_hci_config_t ucfg;
  (void)self;

  if (g.started) return true;

  g.have_target = addr != NULL;
  if (g.have_target) memcpy(g.target, addr, DS_HCI_BD_ADDR_LEN);
  g.connecting = false;
  g.link_up_at = 0;
  memset(&g.cal, 0, sizeof(g.cal));
  bond_load();

  memset(&hcfg, 0, sizeof(hcfg));
  hcfg.send_cmd = ds_usb_hci_send_cmd;
  hcfg.send_acl = ds_usb_hci_send_acl;
  hcfg.on_state = on_host_state;
  hcfg.on_l2cap = on_host_l2cap;
  hcfg.get_link_key = get_link_key;
  hcfg.accept_peer = accept_peer;
  hcfg.on_link_key = new_link_key;
  hcfg.on_device_found = on_device_found;
  hcfg.log = relay_log;
  if (!ds_hci_host_init(&g.host, &hcfg)) return false;

  memset(&lcfg, 0, sizeof(lcfg));
  lcfg.send = l2cap_send;
  lcfg.on_open = on_chan_open;
  lcfg.on_close = on_chan_close;
  lcfg.on_data = on_hid_data;
  lcfg.log = relay_log;
  if (!ds_l2cap_init(&g.l2, &lcfg)) return false;

  memset(&ucfg, 0, sizeof(ucfg));
  ucfg.on_event = on_usb_event;
  ucfg.on_acl = on_usb_acl;
  ucfg.on_attach = on_usb_attach;
  ucfg.on_detach = on_usb_detach;
  ucfg.log = relay_log;
  if (!ds_usb_hci_init(&ucfg)) return false;

  g.lock = xSemaphoreCreateMutex();
  if (g.lock == NULL) {
    relay_log(NULL, "no memory for the stack lock");
    return false;
  }
  g.stop = false;
  if (xTaskCreate(pump_task, "ds5", PUMP_STACK, NULL, PUMP_PRIO, &g.task) !=
      pdPASS) {
    relay_log(NULL, "cannot start the stack task");
    return false;
  }

  g.started = true;
  announce(DS_CONNECTING);
  return true;
}

static void link_end(ds_transport_t *self) {
  (void)self;
  if (!g.started) return;

  /* Ask the pump to finish the iteration it is in and let it go, rather than
   * deleting a task that might be halfway through a USB submission. */
  g.stop = true;
  while (g.task != NULL) vTaskDelay(1);

  ds_l2cap_reset(&g.l2);
  ds_usb_hci_deinit();
  g.started = false;
  g.connecting = false;
  g.link_up_at = 0;
  announce(DS_DISCONNECTED);
}

static void pump(void) {
  ds_usb_hci_poll();

  /* A supervision timeout is reported when the radio gives up, which is many
   * seconds after the pad actually stopped talking. Saying so as it happens
   * puts a timestamp on the real event rather than on the aftermath. */
  if (g.hid_up && !g.reported_quiet &&
      now_ms() - g.last_acl_ms > QUIET_MS) {
    g.reported_quiet = true;
    relay_log(NULL, "pad went quiet");
  }

  /* Guard against a pad that decides to open the channels itself after all:
   * if nothing has appeared on an encrypted link, open them from here. */
  if (g.link_up_at != 0 && now_ms() - g.link_up_at > OPEN_GRACE_MS &&
      ds_hci_host_is_connected(&g.host) &&
      ds_l2cap_channel(&g.l2, DS_L2CAP_PSM_HID_CONTROL) == NULL) {
    g.link_up_at = now_ms();
    ds_l2cap_connect_hid(&g.l2);
  }
}

static void pump_task(void *arg) {
  (void)arg;
  while (!g.stop) {
    take();
    pump();
    give();
    vTaskDelay(PUMP_PERIOD_TICKS);
  }
  g.task = NULL;
  vTaskDelete(NULL);
}

/*
 * Kept because ds_transport_t has the slot and sketches call it, but there is
 * nothing left to do here: the stack runs itself. Leaving it callable means a
 * sketch written against the old behaviour still works.
 */
static void link_poll(ds_transport_t *self) {
  (void)self;
}

static bool link_send(ds_transport_t *self, const uint8_t *report,
                      uint16_t len) {
  bool ok;
  (void)self;
  /* Called from the sketch's task while the pump runs in another. */
  take();
  ok = ds_l2cap_send(&g.l2, DS_L2CAP_PSM_HID_INTERRUPT, report, len);
  give();
  return ok;
}

static ds_conn_state_t link_state(ds_transport_t *self) {
  (void)self;
  if (!g.started) return DS_DISCONNECTED;
  if (ds_l2cap_is_open(&g.l2, DS_L2CAP_PSM_HID_INTERRUPT)) {
    return DS_CONNECTED;
  }
  if (ds_hci_host_state(&g.host) == DS_HOST_FAILED) return DS_LINK_ERROR;
  return DS_CONNECTING;
}

/* ========================================================================== */
/*                                  public                                    */
/* ========================================================================== */

ds_transport_t *ds_link_usb_hci(void) {
  if (g.tr.begin == NULL) {
    g.tr.link = DS_LINK_BT;   /* a dongle carries Bluetooth reports */
    g.tr.begin = link_begin;
    g.tr.end = link_end;
    g.tr.poll = link_poll;
    g.tr.send = link_send;
    g.tr.state = link_state;
    g.tr.calibration = ds_link_usb_hci_calibration;
  g.tr.peer = ds_link_usb_hci_peer;
  }
  return &g.tr;
}

void ds_link_usb_hci_set_bond(ds_transport_t *tr,
                               const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                               const uint8_t key[DS_HCI_LINK_KEY_LEN]) {
  (void)tr;
  if (addr == NULL || key == NULL) return;
  memcpy(g.bond_addr, addr, DS_HCI_BD_ADDR_LEN);
  memcpy(g.bond_key, key, DS_HCI_LINK_KEY_LEN);
  g.have_bond = true;
}

bool ds_link_usb_hci_begin(ds_transport_t *tr, const char *addr) {
  uint8_t bd[DS_HCI_BD_ADDR_LEN];

  if (tr == NULL || tr->begin == NULL) return false;
  if (addr == NULL) return tr->begin(tr, NULL);
  if (!ds_hci_str_to_addr(addr, bd)) {
    relay_log(NULL, "not a bluetooth address");
    return false;
  }
  return tr->begin(tr, bd);
}

void ds_link_usb_hci_persist(ds_transport_t *tr, bool enable) {
  (void)tr;
  g.persist = enable;
  g.persist_set = true;
}

void ds_link_usb_hci_clear_bond(ds_transport_t *tr) {
  (void)tr;
  bond_erase();
  g.have_bond = false;
  memset(g.bond_addr, 0, sizeof(g.bond_addr));
  memset(g.bond_key, 0, sizeof(g.bond_key));
}

void ds_link_usb_hci_on_bond(ds_transport_t *tr,
                              void (*cb)(void *user,
                                         const uint8_t addr[DS_HCI_BD_ADDR_LEN],
                                         const uint8_t key[DS_HCI_LINK_KEY_LEN]),
                              void *user) {
  (void)tr;
  g.on_bond = cb;
  g.bond_user = user;
}

void ds_link_usb_hci_on_log(ds_transport_t *tr,
                             void (*cb)(void *user, const char *msg),
                             void *user) {
  (void)tr;
  g.on_log = cb;
  g.log_user = user;
}

const ds_calibration_t *ds_link_usb_hci_calibration(ds_transport_t *tr) {
  (void)tr;
  return g.cal.valid ? &g.cal : NULL;
}

const uint8_t *ds_link_usb_hci_peer(ds_transport_t *tr) {
  (void)tr;
  return g.host.have_handle ? g.host.peer_addr : NULL;
}

ds_hci_host_t *ds_link_usb_hci_host(ds_transport_t *tr) {
  (void)tr;
  return &g.host;
}

ds_l2cap_t *ds_link_usb_hci_l2cap(ds_transport_t *tr) {
  (void)tr;
  return &g.l2;
}

#endif /* DS_HAS_USB_HCI */

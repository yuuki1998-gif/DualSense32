/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_usb_hci.c — HCI over a USB Bluetooth dongle. See ds_usb_hci.h.
 */
#include "ds_usb_hci.h"

#ifdef DS_HAS_USB_HCI

#include <string.h>

#include "ds_hci.h"
#include "freertos/FreeRTOS.h"
#include "usb/usb_host.h"

/* Largest HCI event is a code, a length and 255 parameters.
 *
 * Events are read one USB packet at a time, not one event at a time. The host
 * controller driver splits an interrupt transfer into packet-sized descriptors
 * and asserts (`_buffer_parse_intr`, "rem_len == 0") if the device ends the
 * transfer early with a short packet — which is exactly what happens when a
 * six byte event arrives into a multi-packet request. So each transfer asks
 * for one packet, and whole events are put back together here. */
#define EVT_BUF        272
/* ACL in: header plus whatever the controller's buffer size turns out to be.
 * A whole number of 64 byte bulk packets, comfortably over any dongle. */
#define ACL_IN_BUF     1024
/* ACL out: the layer above never builds a fragment larger than its own tx
 * buffer, so this only has to match that. */
#define ACL_OUT_BUF    320
#define ACL_OUT_SLOTS  4

#define SLOT_FREE     0
#define SLOT_READY    1
#define SLOT_IN_FLIGHT 2
/* An HCI command is at most 3 + 255, plus the setup packet in front. */
#define CMD_BUF        320
#define CMD_QUEUE      4

typedef struct {
  uint8_t data[DS_HCI_CMD_MAX_LEN];
  uint16_t len;
} queued_cmd_t;

static struct {
  ds_usb_hci_config_t cfg;
  bool installed;
  usb_host_client_handle_t client;
  usb_device_handle_t dev;
  bool attached;
  uint16_t vid, pid;

  uint8_t evt_ep, acl_in_ep, acl_out_ep;
  uint16_t evt_mps;             /* one packet is one transfer */
  uint8_t evt_acc[EVT_BUF];     /* event under construction   */
  uint16_t evt_acc_len;

  usb_transfer_t *cmd_xfer;
  usb_transfer_t *evt_xfer;
  usb_transfer_t *acl_in_xfer;
  usb_transfer_t *acl_out_xfer[ACL_OUT_SLOTS];

  volatile bool cmd_busy;
  queued_cmd_t queue[CMD_QUEUE];
  volatile uint8_t q_head, q_tail;

  /* Set from a transfer callback, acted on by poll: resubmitting an inbound
   * transfer from inside its own callback works, but doing it after the
   * callback returns keeps the failure path in one place. */
  volatile bool evt_idle, acl_in_idle;

  /* An outbound ACL slot is free, filled and waiting to go, or in flight.
   * The middle state is the point: it lets a send happen inside a callback
   * without the submission happening there too. */
  volatile uint8_t acl_out_state[ACL_OUT_SLOTS];
  uint16_t acl_out_len[ACL_OUT_SLOTS];

  /* Packets whose transfers have completed but whose owner has not been told
   * yet. Telling it is what makes it send the next one, so that also waits
   * until the callbacks are done. */
  volatile uint8_t sent_pending;

  uint32_t last_scan;
} g;

static void say(const char *msg) {
  if (g.cfg.log != NULL) g.cfg.log(g.cfg.ctx, msg);
}

/* ========================================================================== */
/*                            descriptor walking                              */
/* ========================================================================== */

/* Collects the three endpoints of the first HCI interface. Returns false if
 * the device does not have all of them, which means it is not what it claims
 * to be and is better rejected than half-driven. */
static bool find_endpoints(const usb_config_desc_t *cfg) {
  const uint8_t *p = (const uint8_t *)cfg;
  uint16_t total = cfg->wTotalLength;
  uint16_t off = 0;
  bool in_hci_intf = false;

  g.evt_ep = g.acl_in_ep = g.acl_out_ep = 0;

  while (off + 2 <= total) {
    uint8_t len = p[off];
    uint8_t type = p[off + 1];
    if (len < 2 || off + len > total) break;

    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *i = (const usb_intf_desc_t *)(p + off);
      /* Only alternate setting 0 of the first interface. Interface 1 has the
       * same class triple but carries SCO audio on isochronous endpoints. */
      in_hci_intf = i->bInterfaceNumber == 0 && i->bAlternateSetting == 0 &&
                    i->bInterfaceClass == DS_HCI_USB_CLASS &&
                    i->bInterfaceSubClass == DS_HCI_USB_SUBCLASS &&
                    i->bInterfaceProtocol == DS_HCI_USB_PROTOCOL;
    } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_hci_intf) {
      const usb_ep_desc_t *e = (const usb_ep_desc_t *)(p + off);
      uint8_t xfer = e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
      bool is_in = (e->bEndpointAddress & 0x80) != 0;

      if (xfer == USB_BM_ATTRIBUTES_XFER_INT && is_in) {
        g.evt_ep = e->bEndpointAddress;
        g.evt_mps = e->wMaxPacketSize;
      } else if (xfer == USB_BM_ATTRIBUTES_XFER_BULK) {
        if (is_in) {
          g.acl_in_ep = e->bEndpointAddress;
        } else {
          g.acl_out_ep = e->bEndpointAddress;
        }
      }
    }
    off = (uint16_t)(off + len);
  }
  return g.evt_ep != 0 && g.evt_mps != 0 && g.acl_in_ep != 0 &&
         g.acl_out_ep != 0;
}

/* ========================================================================== */
/*                             transfer callbacks                             */
/* ========================================================================== */

static void say_sent(void) {
  if (g.cfg.on_sent != NULL) g.cfg.on_sent(g.cfg.ctx);
}

static void on_cmd_done(usb_transfer_t *t) {
  (void)t;
  g.cmd_busy = false;
  g.sent_pending++;
}

/* One USB packet in, zero or more whole HCI events out. An event that does
 * not fit in a packet arrives as several, and one whose length happens to be
 * a multiple of the packet size is followed by an empty packet. */
static void on_evt_done(usb_transfer_t *t) {
  uint16_t n;

  if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
    g.evt_acc_len = 0;   /* a failed packet leaves a partial event behind */
    g.evt_idle = true;
    return;
  }
  n = (uint16_t)t->actual_num_bytes;
  if (n != 0) {
    if ((uint32_t)g.evt_acc_len + n <= sizeof(g.evt_acc)) {
      memcpy(g.evt_acc + g.evt_acc_len, t->data_buffer, n);
      g.evt_acc_len = (uint16_t)(g.evt_acc_len + n);
    } else {
      say("usb: oversized event dropped");
      g.evt_acc_len = 0;
    }
  }

  while (g.evt_acc_len >= DS_HCI_EVT_HDR_LEN) {
    uint16_t total = (uint16_t)(DS_HCI_EVT_HDR_LEN + g.evt_acc[1]);
    if (g.evt_acc_len < total) break;
    if (g.cfg.on_event != NULL) g.cfg.on_event(g.cfg.ctx, g.evt_acc, total);
    g.evt_acc_len = (uint16_t)(g.evt_acc_len - total);
    if (g.evt_acc_len != 0) {
      memmove(g.evt_acc, g.evt_acc + total, g.evt_acc_len);
    }
  }
  g.evt_idle = true;
}

static void on_acl_in_done(usb_transfer_t *t) {
  if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0 &&
      g.cfg.on_acl != NULL) {
    g.cfg.on_acl(g.cfg.ctx, t->data_buffer, (uint16_t)t->actual_num_bytes);
  }
  g.acl_in_idle = true;
}

static void on_acl_out_done(usb_transfer_t *t) {
  int i;
  for (i = 0; i < ACL_OUT_SLOTS; i++) {
    if (g.acl_out_xfer[i] == t) {
      g.acl_out_state[i] = SLOT_FREE;
      break;
    }
  }
  t->context = NULL;
  g.sent_pending++;
}

/* ========================================================================== */
/*                                  pipes                                     */
/* ========================================================================== */

static void submit_evt(void) {
  if (!g.attached || g.evt_xfer == NULL) return;
  g.evt_xfer->device_handle = g.dev;
  g.evt_xfer->bEndpointAddress = g.evt_ep;
  /* Exactly one packet: see the note on EVT_BUF. */
  g.evt_xfer->num_bytes = g.evt_mps;
  g.evt_xfer->callback = on_evt_done;
  g.evt_xfer->context = NULL;
  if (usb_host_transfer_submit(g.evt_xfer) != ESP_OK) {
    g.evt_idle = true;   /* try again next poll rather than going deaf */
  }
}

static void submit_acl_in(void) {
  if (!g.attached || g.acl_in_xfer == NULL) return;
  g.acl_in_xfer->device_handle = g.dev;
  g.acl_in_xfer->bEndpointAddress = g.acl_in_ep;
  g.acl_in_xfer->num_bytes = ACL_IN_BUF;
  g.acl_in_xfer->callback = on_acl_in_done;
  g.acl_in_xfer->context = NULL;
  if (usb_host_transfer_submit(g.acl_in_xfer) != ESP_OK) {
    g.acl_in_idle = true;
  }
}

/* Sends the command at the head of the queue, if the control pipe is free. */
static void pump_commands(void) {
  usb_setup_packet_t *s;
  queued_cmd_t *c;

  if (!g.attached || g.cmd_busy || g.q_head == g.q_tail) return;

  c = &g.queue[g.q_tail];
  s = (usb_setup_packet_t *)g.cmd_xfer->data_buffer;
  /* A class request to the device, with the command itself as the data stage.
   * There is no reply here: the answer comes back as an event. */
  s->bmRequestType = DS_HCI_USB_BMREQUESTTYPE;
  s->bRequest = DS_HCI_USB_BREQUEST;
  s->wValue = DS_HCI_USB_WVALUE;
  s->wIndex = DS_HCI_USB_WINDEX;
  s->wLength = c->len;
  memcpy(g.cmd_xfer->data_buffer + sizeof(usb_setup_packet_t), c->data, c->len);

  g.cmd_xfer->device_handle = g.dev;
  g.cmd_xfer->bEndpointAddress = 0;
  g.cmd_xfer->num_bytes = (int)(sizeof(usb_setup_packet_t) + c->len);
  g.cmd_xfer->callback = on_cmd_done;
  g.cmd_xfer->context = NULL;

  g.cmd_busy = true;
  if (usb_host_transfer_submit_control(g.client, g.cmd_xfer) != ESP_OK) {
    g.cmd_busy = false;
    say("usb: command submit failed");
    return;   /* leave it queued and try again */
  }
  g.q_tail = (uint8_t)((g.q_tail + 1) % CMD_QUEUE);
}

/* ========================================================================== */
/*                            attach and detach                               */
/* ========================================================================== */

static bool alloc_transfers(void) {
  int i;
  if (usb_host_transfer_alloc(CMD_BUF, 0, &g.cmd_xfer) != ESP_OK) return false;
  if (usb_host_transfer_alloc(EVT_BUF, 0, &g.evt_xfer) != ESP_OK) return false;
  if (usb_host_transfer_alloc(ACL_IN_BUF, 0, &g.acl_in_xfer) != ESP_OK) {
    return false;
  }
  for (i = 0; i < ACL_OUT_SLOTS; i++) {
    if (usb_host_transfer_alloc(ACL_OUT_BUF, 0, &g.acl_out_xfer[i]) != ESP_OK) {
      return false;
    }
    g.acl_out_xfer[i]->context = NULL;
  }
  return true;
}

static void free_transfers(void) {
  int i;
  if (g.cmd_xfer) { usb_host_transfer_free(g.cmd_xfer); g.cmd_xfer = NULL; }
  if (g.evt_xfer) { usb_host_transfer_free(g.evt_xfer); g.evt_xfer = NULL; }
  if (g.acl_in_xfer) {
    usb_host_transfer_free(g.acl_in_xfer);
    g.acl_in_xfer = NULL;
  }
  for (i = 0; i < ACL_OUT_SLOTS; i++) {
    if (g.acl_out_xfer[i]) {
      usb_host_transfer_free(g.acl_out_xfer[i]);
      g.acl_out_xfer[i] = NULL;
    }
  }
}

static void detach(void) {
  if (!g.attached && g.dev == NULL) return;

  g.attached = false;
  free_transfers();
  if (g.dev != NULL) {
    usb_host_interface_release(g.client, g.dev, 0);
    usb_host_device_close(g.client, g.dev);
    g.dev = NULL;
  }
  g.cmd_busy = false;
  g.q_head = g.q_tail = 0;
  say("usb: dongle gone");
  if (g.cfg.on_detach != NULL) g.cfg.on_detach(g.cfg.ctx);
}

static bool try_attach(uint8_t addr) {
  const usb_device_desc_t *dd = NULL;
  const usb_config_desc_t *cd = NULL;

  if (g.attached) return false;
  if (usb_host_device_open(g.client, addr, &g.dev) != ESP_OK) return false;

  if (usb_host_get_device_descriptor(g.dev, &dd) != ESP_OK || dd == NULL) {
    goto reject;
  }
  if (g.cfg.vid != 0 || g.cfg.pid != 0) {
    if (dd->idVendor != g.cfg.vid || dd->idProduct != g.cfg.pid) goto reject;
  }
  /* A dongle declares the Bluetooth class on the device descriptor as well as
   * on its interfaces. Checking here rejects keyboards and the like before
   * we bother walking their descriptors. */
  if (dd->bDeviceClass != DS_HCI_USB_CLASS ||
      dd->bDeviceSubClass != DS_HCI_USB_SUBCLASS ||
      dd->bDeviceProtocol != DS_HCI_USB_PROTOCOL) {
    goto reject;
  }

  /* Null here means the device is addressed but was never configured, which
   * happens when the enumeration filter callback is left unset. */
  if (usb_host_get_active_config_descriptor(g.dev, &cd) != ESP_OK ||
      cd == NULL) {
    say("usb: no active configuration");
    goto reject;
  }
  if (!find_endpoints(cd)) {
    say("usb: hci interface is missing an endpoint");
    goto reject;
  }
  if (usb_host_interface_claim(g.client, g.dev, 0, 0) != ESP_OK) {
    say("usb: could not claim interface 0");
    goto reject;
  }
  if (!alloc_transfers()) {
    say("usb: out of memory for transfers");
    usb_host_interface_release(g.client, g.dev, 0);
    goto reject;
  }

  g.vid = dd->idVendor;
  g.pid = dd->idProduct;
  g.attached = true;
  g.evt_acc_len = 0;
  g.evt_idle = true;
  g.acl_in_idle = true;
  g.sent_pending = 0;
  memset((void *)g.acl_out_state, SLOT_FREE, sizeof(g.acl_out_state));
  g.cmd_busy = false;
  g.q_head = g.q_tail = 0;
  say("usb: dongle attached");
  if (g.cfg.on_attach != NULL) g.cfg.on_attach(g.cfg.ctx, g.vid, g.pid);
  return true;

reject:
  usb_host_device_close(g.client, g.dev);
  g.dev = NULL;
  return false;
}

/* ========================================================================== */
/*                              host library                                  */
/* ========================================================================== */

/* Called during enumeration once the device descriptor is in. It must not
 * block and must not submit transfers, so it only names a configuration.
 *
 * Leaving this unset is not the harmless default it looks like: this build of
 * the host library has the filter compiled in, and without a callback it
 * addresses the device and then never configures it. */
static bool enum_filter(const usb_device_desc_t *dd, uint8_t *config_value) {
  (void)dd;
  *config_value = 1;
  return true;
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg) {
  (void)arg;
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      try_attach(msg->new_dev.address);
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      detach();
      break;
    default:
      break;
  }
}

bool ds_usb_hci_init(const ds_usb_hci_config_t *cfg) {
  usb_host_config_t host_cfg;
  usb_host_client_config_t client_cfg;

  if (cfg == NULL) return false;
  memset(&g, 0, sizeof(g));
  g.cfg = *cfg;

  memset(&host_cfg, 0, sizeof(host_cfg));
  host_cfg.skip_phy_setup = false;
  host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  host_cfg.enum_filter_cb = enum_filter;
  if (usb_host_install(&host_cfg) != ESP_OK) {
    say("usb: host install failed");
    return false;
  }
  g.installed = true;

  memset(&client_cfg, 0, sizeof(client_cfg));
  client_cfg.is_synchronous = false;
  client_cfg.max_num_event_msg = 5;
  client_cfg.async.client_event_callback = client_cb;
  client_cfg.async.callback_arg = NULL;
  if (usb_host_client_register(&client_cfg, &g.client) != ESP_OK) {
    say("usb: client register failed");
    usb_host_uninstall();
    g.installed = false;
    return false;
  }
  return true;
}

void ds_usb_hci_deinit(void) {
  detach();
  if (g.client != NULL) {
    usb_host_client_deregister(g.client);
    g.client = NULL;
  }
  if (g.installed) {
    usb_host_uninstall();
    g.installed = false;
  }
}

/* Defined below, next to the send it drains. */
static void submit_acl_out(void);

void ds_usb_hci_poll(void) {
  uint32_t flags = 0;

  if (!g.installed) return;
  usb_host_lib_handle_events(0, &flags);
  usb_host_client_handle_events(g.client, 0);

  if (g.attached) {
    /* Order matters. Telling the owner its packets went out is what makes it
     * queue the next ones, so do that first and submit everything after. */
    while (g.sent_pending > 0) {
      g.sent_pending--;
      say_sent();
    }
    if (g.evt_idle) { g.evt_idle = false; submit_evt(); }
    if (g.acl_in_idle) { g.acl_in_idle = false; submit_acl_in(); }
    submit_acl_out();
    pump_commands();
    return;
  }

  /* A device that was already sitting on the bus at power-up can finish
   * enumerating before this client registers, and then no new-device event
   * ever arrives. Sweep for one now and then. */
  {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint8_t addrs[4];
    int n = 0;
    if (now - g.last_scan < 1000) return;
    g.last_scan = now;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK) {
      int i;
      for (i = 0; i < n && !g.attached; i++) try_attach(addrs[i]);
    }
  }
}

bool ds_usb_hci_attached(void) {
  return g.attached;
}

/* ========================================================================== */
/*                                 sending                                    */
/* ========================================================================== */

int ds_usb_hci_send_cmd(void *ctx, const uint8_t *pkt, uint16_t len) {
  uint8_t next;
  (void)ctx;

  if (!g.attached || pkt == NULL || len == 0) return -1;
  if (len > DS_HCI_CMD_MAX_LEN) return -1;

  /* Queue rather than submit: only one control transfer fits on the pipe at a
   * time, and the layer above should not have to care. */
  next = (uint8_t)((g.q_head + 1) % CMD_QUEUE);
  if (next == g.q_tail) {
    say("usb: command queue full");
    return -1;
  }
  memcpy(g.queue[g.q_head].data, pkt, len);
  g.queue[g.q_head].len = len;
  g.q_head = next;

  /* Not sent here. This can be reached from inside a completion callback,
   * and the control pipe must not be handed a transfer from in there. */
  return 0;
}

bool ds_usb_hci_can_send_cmd(void) {
  uint8_t next;
  if (!g.attached) return false;
  next = (uint8_t)((g.q_head + 1) % CMD_QUEUE);
  return next != g.q_tail;
}

bool ds_usb_hci_can_send_acl(void) {
  int i;
  if (!g.attached) return false;
  for (i = 0; i < ACL_OUT_SLOTS; i++) {
    if (g.acl_out_xfer[i] != NULL && g.acl_out_state[i] == SLOT_FREE) {
      return true;
    }
  }
  return false;
}

int ds_usb_hci_send_acl(void *ctx, const uint8_t *pkt, uint16_t len) {
  int i;
  (void)ctx;

  if (!g.attached || pkt == NULL || len == 0) return -1;
  if (len > ACL_OUT_BUF) return -1;

  for (i = 0; i < ACL_OUT_SLOTS; i++) {
    usb_transfer_t *t = g.acl_out_xfer[i];
    if (t == NULL || g.acl_out_state[i] != SLOT_FREE) continue;

    memcpy(t->data_buffer, pkt, len);
    g.acl_out_len[i] = len;
    g.acl_out_state[i] = SLOT_READY;
    return 0;
  }
  say("usb: no free acl transfer");
  return -1;
}

/* Everything queued since the last time round, handed to the driver now that
 * no callback is on the stack. */
static void submit_acl_out(void) {
  int i;
  for (i = 0; i < ACL_OUT_SLOTS; i++) {
    usb_transfer_t *t = g.acl_out_xfer[i];
    if (t == NULL || g.acl_out_state[i] != SLOT_READY) continue;

    t->device_handle = g.dev;
    t->bEndpointAddress = g.acl_out_ep;
    t->num_bytes = g.acl_out_len[i];
    t->callback = on_acl_out_done;
    t->context = t;
    g.acl_out_state[i] = SLOT_IN_FLIGHT;
    if (usb_host_transfer_submit(t) != ESP_OK) {
      say("usb: acl submit failed");
      g.acl_out_state[i] = SLOT_FREE;
      t->context = NULL;
      g.sent_pending++;   /* or the owner waits for a packet that never was */
    }
  }
}

#endif /* DS_HAS_USB_HCI */

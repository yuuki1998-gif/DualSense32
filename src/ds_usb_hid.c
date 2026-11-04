/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 yuuki1998
 */
/*
 * ds_usb_hid.c — a DualSense on a wire. See ds_usb_hid.h.
 */
#include "ds_usb_hid.h"

#ifdef DS_HAS_USB_HID

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "usb/usb_host.h"

/* HID class requests, from the USB HID specification. wIndex is the interface
 * number, not zero — a device with several interfaces will refuse otherwise. */
#define HID_CLASS              0x03
#define HID_GET_REPORT         0x01
#define HID_SET_REPORT         0x09
#define HID_REQ_TYPE_GET       0xA1   /* IN,  class, recipient interface */
#define HID_REQ_TYPE_SET       0x21   /* OUT, class, recipient interface */
#define HID_REPORT_TYPE_FEATURE 0x03

#define IN_BUF   64     /* the DualSense input report is exactly this */
#define OUT_BUF  64
#define CTRL_BUF 256
#define EP0_MPS  64

static struct {
  ds_usb_hid_config_t cfg;
  bool installed;
  usb_host_client_handle_t client;
  usb_device_handle_t dev;
  bool attached;
  uint16_t vid, pid;

  uint8_t intf;                 /* the HID interface number */
  uint8_t in_ep, out_ep;
  uint16_t in_mps;

  usb_transfer_t *in_xfer;
  usb_transfer_t *out_xfer;
  usb_transfer_t *ctrl_xfer;

  volatile bool in_idle;
  volatile bool out_busy;
  volatile bool ctrl_done;
  volatile int ctrl_status;
  volatile int ctrl_bytes;

  uint32_t last_scan;
} g;

static void say(const char *msg) {
  if (g.cfg.log != NULL) g.cfg.log(g.cfg.ctx, msg);
}

/* ========================================================================== */
/*                            descriptor walking                              */
/* ========================================================================== */

/* Finds the HID interface and its endpoints. Searching by class rather than
 * assuming a number keeps this working across controller revisions, which do
 * not all put HID in the same slot. */
static bool find_hid(const usb_config_desc_t *cfg) {
  const uint8_t *p = (const uint8_t *)cfg;
  uint16_t total = cfg->wTotalLength;
  uint16_t off = 0;
  bool in_hid = false;

  g.in_ep = g.out_ep = 0;
  g.in_mps = 0;

  while (off + 2 <= total) {
    uint8_t len = p[off];
    uint8_t type = p[off + 1];
    if (len < 2 || off + len > total) break;

    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *i = (const usb_intf_desc_t *)(p + off);
      /* Stop at the first HID interface that has already given us an input
       * endpoint, so a later interface cannot overwrite it. */
      if (in_hid && g.in_ep != 0) break;
      in_hid = i->bInterfaceClass == HID_CLASS && i->bAlternateSetting == 0;
      if (in_hid) g.intf = i->bInterfaceNumber;
    } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_hid) {
      const usb_ep_desc_t *e = (const usb_ep_desc_t *)(p + off);
      if ((e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) !=
          USB_BM_ATTRIBUTES_XFER_INT) {
        /* not an interrupt endpoint; ignore */
      } else if (e->bEndpointAddress & 0x80) {
        g.in_ep = e->bEndpointAddress;
        g.in_mps = e->wMaxPacketSize;
      } else {
        g.out_ep = e->bEndpointAddress;
      }
    }
    off = (uint16_t)(off + len);
  }
  /* An output endpoint is optional: a pad without one still reports, it just
   * cannot be driven over the interrupt pipe. */
  return g.in_ep != 0 && g.in_mps != 0;
}

/* ========================================================================== */
/*                             transfer callbacks                             */
/* ========================================================================== */

static void on_in_done(usb_transfer_t *t) {
  if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0 &&
      g.cfg.on_report != NULL) {
    g.cfg.on_report(g.cfg.ctx, t->data_buffer, (uint16_t)t->actual_num_bytes);
  }
  g.in_idle = true;
}

static void on_out_done(usb_transfer_t *t) {
  (void)t;
  g.out_busy = false;
}

static void on_ctrl_done(usb_transfer_t *t) {
  g.ctrl_status = t->status;
  g.ctrl_bytes = t->actual_num_bytes;
  g.ctrl_done = true;
}

static void submit_in(void) {
  if (!g.attached || g.in_xfer == NULL) return;
  g.in_xfer->device_handle = g.dev;
  g.in_xfer->bEndpointAddress = g.in_ep;
  g.in_xfer->num_bytes = g.in_mps;
  g.in_xfer->callback = on_in_done;
  g.in_xfer->context = NULL;
  if (usb_host_transfer_submit(g.in_xfer) != ESP_OK) g.in_idle = true;
}

/* ========================================================================== */
/*                            attach and detach                               */
/* ========================================================================== */

static bool alloc_transfers(void) {
  if (usb_host_transfer_alloc(IN_BUF, 0, &g.in_xfer) != ESP_OK) return false;
  if (usb_host_transfer_alloc(OUT_BUF, 0, &g.out_xfer) != ESP_OK) return false;
  if (usb_host_transfer_alloc(CTRL_BUF, 0, &g.ctrl_xfer) != ESP_OK) return false;
  return true;
}

static void free_transfers(void) {
  if (g.in_xfer) { usb_host_transfer_free(g.in_xfer); g.in_xfer = NULL; }
  if (g.out_xfer) { usb_host_transfer_free(g.out_xfer); g.out_xfer = NULL; }
  if (g.ctrl_xfer) { usb_host_transfer_free(g.ctrl_xfer); g.ctrl_xfer = NULL; }
}

static void detach(void) {
  if (!g.attached && g.dev == NULL) return;
  g.attached = false;
  free_transfers();
  if (g.dev != NULL) {
    usb_host_interface_release(g.client, g.dev, g.intf);
    usb_host_device_close(g.client, g.dev);
    g.dev = NULL;
  }
  say("usb: controller unplugged");
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
  } else if (dd->idVendor != DS_USB_VID_SONY) {
    goto reject;
  }
  /* Null here means the device was addressed but never configured, which
   * happens when the enumeration filter callback is left unset. */
  if (usb_host_get_active_config_descriptor(g.dev, &cd) != ESP_OK ||
      cd == NULL) {
    say("usb: no active configuration");
    goto reject;
  }
  if (!find_hid(cd)) {
    say("usb: no hid interface");
    goto reject;
  }
  if (usb_host_interface_claim(g.client, g.dev, g.intf, 0) != ESP_OK) {
    say("usb: could not claim the hid interface");
    goto reject;
  }
  if (!alloc_transfers()) {
    say("usb: out of memory for transfers");
    usb_host_interface_release(g.client, g.dev, g.intf);
    goto reject;
  }

  g.vid = dd->idVendor;
  g.pid = dd->idProduct;
  g.attached = true;
  g.in_idle = true;
  g.out_busy = false;
  say("usb: controller attached");
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

/* Without this the host library addresses the device and never configures it,
 * because this build has the enumeration filter compiled in. */
static bool enum_filter(const usb_device_desc_t *dd, uint8_t *config_value) {
  (void)dd;
  *config_value = 1;
  return true;
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg) {
  (void)arg;
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    try_attach(msg->new_dev.address);
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    detach();
  }
}

bool ds_usb_hid_init(const ds_usb_hid_config_t *cfg) {
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

void ds_usb_hid_deinit(void) {
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

void ds_usb_hid_poll(void) {
  uint32_t flags = 0;

  if (!g.installed) return;
  usb_host_lib_handle_events(0, &flags);
  usb_host_client_handle_events(g.client, 0);

  if (g.attached) {
    if (g.in_idle) { g.in_idle = false; submit_in(); }
    return;
  }
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

bool ds_usb_hid_attached(void) {
  return g.attached;
}

/* ========================================================================== */
/*                                 sending                                    */
/* ========================================================================== */

bool ds_usb_hid_send(const uint8_t *report, uint16_t len) {
  if (!g.attached || report == NULL || len == 0) return false;
  if (g.out_ep == 0 || len > OUT_BUF) return false;
  if (g.out_busy) return false;   /* one in flight is enough at report rates */

  memcpy(g.out_xfer->data_buffer, report, len);
  g.out_xfer->device_handle = g.dev;
  g.out_xfer->bEndpointAddress = g.out_ep;
  g.out_xfer->num_bytes = len;
  g.out_xfer->callback = on_out_done;
  g.out_xfer->context = NULL;
  g.out_busy = true;
  if (usb_host_transfer_submit(g.out_xfer) != ESP_OK) {
    g.out_busy = false;
    return false;
  }
  return true;
}

int ds_usb_hid_get_feature(uint8_t report_id, uint8_t *buf, uint16_t len,
                            uint32_t timeout_ms) {
  usb_setup_packet_t *s;
  uint32_t t0, flags;
  uint16_t rounded;
  int got;

  if (!g.attached || buf == NULL || len == 0) return -1;
  /* An IN transfer's data stage has to be a whole number of packets. */
  rounded = (uint16_t)(((len + EP0_MPS - 1) / EP0_MPS) * EP0_MPS);
  if (sizeof(usb_setup_packet_t) + rounded > CTRL_BUF) return -1;

  s = (usb_setup_packet_t *)g.ctrl_xfer->data_buffer;
  s->bmRequestType = HID_REQ_TYPE_GET;
  s->bRequest = HID_GET_REPORT;
  s->wValue = (uint16_t)((HID_REPORT_TYPE_FEATURE << 8) | report_id);
  s->wIndex = g.intf;          /* the interface, not zero */
  s->wLength = len;
  g.ctrl_xfer->device_handle = g.dev;
  g.ctrl_xfer->bEndpointAddress = 0;
  g.ctrl_xfer->num_bytes = (int)(sizeof(usb_setup_packet_t) + rounded);
  g.ctrl_xfer->callback = on_ctrl_done;
  g.ctrl_xfer->context = NULL;
  g.ctrl_done = false;

  if (usb_host_transfer_submit_control(g.client, g.ctrl_xfer) != ESP_OK) {
    say("usb: feature request refused");
    return -1;
  }
  t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
  while (!g.ctrl_done) {
    if (xTaskGetTickCount() * portTICK_PERIOD_MS - t0 > timeout_ms) {
      say("usb: feature request timed out");
      return -1;
    }
    usb_host_lib_handle_events(0, &flags);
    usb_host_client_handle_events(g.client, 1);
  }
  if (g.ctrl_status != USB_TRANSFER_STATUS_COMPLETED) return -1;

  got = g.ctrl_bytes - (int)sizeof(usb_setup_packet_t);
  if (got <= 0) return -1;
  if (got > (int)len) got = (int)len;
  memcpy(buf, g.ctrl_xfer->data_buffer + sizeof(usb_setup_packet_t),
         (size_t)got);
  return got;
}

#endif /* DS_HAS_USB_HID */

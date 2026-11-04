/*
 * WhatIsOnUSB — what is actually on the USB port, and if nothing, why.
 *
 * Before writing a line of transport code, find out whether the board can see
 * the device at all and what it says about itself. This answers what guesswork
 * cannot:
 *
 *   1. Are the data lines even connected? A powered full-speed device holds
 *      D+ high through a 1.5k pull-up. If that is missing, no amount of
 *      software will help — the usual culprit is a charge-only cable.
 *   2. Does the controller see a connection, and at what speed?
 *   3. Is it a standard HCI device, class 0xE0 / 0x01 / 0x01? That is the
 *      difference between the plan working and needing a vendor transport.
 *   4. What are the real endpoint addresses?
 *
 * Flash this over the UART port and plug the device into the OTG port, so the
 * log survives. On an ESP32-S3-DevKitC-1 those are the two separate USB-C
 * sockets.
 *
 * Nothing here is DualSense specific — it will describe whatever you plug in.
 */

#include "soc/soc_caps.h"

#if !SOC_USB_OTG_SUPPORTED

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("This chip has no USB OTG. Use an ESP32-S3 or S2.");
}
void loop() { delay(1000); }

#else

#include "usb/usb_host.h"
#include "soc/usb_dwc_struct.h"
#include "esp_log.h"

/* The S3 routes its internal USB PHY to these two pads. */
#define PIN_DMINUS 19
#define PIN_DPLUS  20

static usb_host_client_handle_t client = NULL;
static volatile uint8_t pendingAddr = 0;
static usb_device_handle_t openDev = NULL;
static uint32_t lastNag = 0;

/* ------------------------------------------------------------ diagnosis */

/* Read before the USB peripheral takes the pads. A full-speed device that has
 * power pulls D+ up to 3.3 V through 1.5k, which easily beats the chip's own
 * ~45k pull-down. Low on both means the device is unpowered or the data lines
 * are not there. */
static void checkPins() {
  pinMode(PIN_DMINUS, INPUT_PULLDOWN);
  pinMode(PIN_DPLUS, INPUT_PULLDOWN);
  delay(50);
  int dm = digitalRead(PIN_DMINUS);
  int dp = digitalRead(PIN_DPLUS);

  Serial.printf("line check   D+ (gpio%d) = %d   D- (gpio%d) = %d\n",
                PIN_DPLUS, dp, PIN_DMINUS, dm);
  if (dp && !dm) {
    Serial.println("  -> full speed device attached, data lines connected");
  } else if (!dp && dm) {
    Serial.println("  -> low speed device attached, data lines connected");
  } else if (dp && dm) {
    Serial.println("  -> both lines high, which is not a valid idle state");
  } else {
    Serial.println("  -> no pull-up seen. Either the device has no power, or");
    Serial.println("     the data lines are not connected (charge-only cable");
    Serial.println("     or adapter). A lit LED only proves power, not data.");
  }
}

static void dumpPort() {
  usb_dwc_hprt_reg_t h;
  usb_host_lib_info_t info;
  const char* spd[] = {"high", "full", "low", "?"};
  const char* lines[] = {"both low", "D+ high", "D- high", "both high"};

  h.val = USB_DWC.hprt_reg.val;
  Serial.printf("port: power=%u connected=%u enabled=%u reset=%u "
                "speed=%s lines=%s overcurrent=%u\n",
                h.prtpwr, h.prtconnsts, h.prtena, h.prtrst,
                spd[h.prtspd & 3], lines[h.prtlnsts & 3], h.prtovrcurract);

  if (usb_host_lib_info(&info) == ESP_OK) {
    Serial.printf("lib:  %d devices, %d clients\n", info.num_devices,
                  info.num_clients);
  }
  if (!h.prtpwr) {
    Serial.println("  -> the root port is not powered");
  } else if (!h.prtconnsts) {
    Serial.println("  -> port is powered but the controller sees nothing on");
    Serial.println("     the wire. Check the data lines before anything else.");
  } else if (info.num_devices == 0) {
    Serial.println("  -> a device is attached but enumeration did not finish");
  }
}

/* ------------------------------------------------------------- printing */

static const char* xferName(uint8_t attr) {
  switch (attr & USB_BM_ATTRIBUTES_XFERTYPE_MASK) {
    case USB_BM_ATTRIBUTES_XFER_CONTROL: return "control";
    case USB_BM_ATTRIBUTES_XFER_ISOC:    return "isochronous";
    case USB_BM_ATTRIBUTES_XFER_BULK:    return "bulk";
    case USB_BM_ATTRIBUTES_XFER_INT:     return "interrupt";
    default:                             return "?";
  }
}

/* String descriptors are UTF-16LE. Everything we care about is ASCII. */
static void printStr(const char* label, const usb_str_desc_t* s) {
  Serial.printf("  %-14s ", label);
  if (s == NULL || s->bLength < 4) { Serial.println("(none)"); return; }
  int n = (s->bLength - 2) / 2;
  for (int i = 0; i < n; i++) {
    uint16_t c = s->wData[i];
    Serial.print((c >= 0x20 && c < 0x7F) ? (char)c : '.');
  }
  Serial.println();
}

/* Walks the configuration descriptor as raw bytes. Every descriptor starts
 * with its own length and type, so no parser library is needed. */
static void dumpConfig(const usb_config_desc_t* cfg) {
  const uint8_t* p = (const uint8_t*)cfg;
  uint16_t total = cfg->wTotalLength;
  uint16_t off = 0;
  int hciInterfaces = 0;

  Serial.printf("\nconfiguration: %u interfaces, %u mA\n",
                cfg->bNumInterfaces, cfg->bMaxPower * 2);

  while (off + 2 <= total) {
    uint8_t len = p[off];
    uint8_t type = p[off + 1];
    if (len < 2 || off + len > total) break;

    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* i = (const usb_intf_desc_t*)(p + off);
      bool isHci = i->bInterfaceClass == 0xE0 &&
                   i->bInterfaceSubClass == 0x01 &&
                   i->bInterfaceProtocol == 0x01;
      if (isHci) hciInterfaces++;
      Serial.printf("\n  interface %u alt %u  class %02X/%02X/%02X  %u endpoints%s\n",
                    i->bInterfaceNumber, i->bAlternateSetting,
                    i->bInterfaceClass, i->bInterfaceSubClass,
                    i->bInterfaceProtocol, i->bNumEndpoints,
                    isHci ? "   <-- Bluetooth HCI" : "");
    } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t* e = (const usb_ep_desc_t*)(p + off);
      Serial.printf("    ep 0x%02X  %-11s  %s  max %u  interval %u\n",
                    e->bEndpointAddress, xferName(e->bmAttributes),
                    (e->bEndpointAddress & 0x80) ? "IN " : "OUT",
                    e->wMaxPacketSize, e->bInterval);
    }
    off += len;
  }

  Serial.println();
  if (hciInterfaces == 0) {
    Serial.println("VERDICT: not a standard HCI device. A vendor-specific");
    Serial.println("         transport would be needed.");
  } else {
    Serial.println("VERDICT: standard Bluetooth HCI device.");
    Serial.println("         Interface 0 carries commands on the control pipe,");
    Serial.println("         events on interrupt IN and ACL on the bulk pair.");
    Serial.println("         Interface 1 is SCO audio and is not needed.");
  }
}

/* --------------------------------------------------- manual control read */

/* The host library gives up before it publishes an active configuration, but
 * endpoint zero clearly works — the device descriptor came back over it. So
 * ask the device ourselves. This is also the first real use of the control
 * pipe, which is exactly how HCI commands will go out later. */

static volatile bool ctrlDone = false;
static volatile int ctrlStatus = -1;
static volatile int ctrlBytes = 0;
static usb_transfer_t* ctrlXfer = NULL;

#define CTRL_BUF 1024
#define EP0_MPS  64

static void ctrlCb(usb_transfer_t* t) {
  ctrlStatus = t->status;
  ctrlBytes = t->actual_num_bytes;
  ctrlDone = true;
}

/* Returns the number of data bytes read, or -1. */
static int getDescriptor(uint16_t wantLen) {
  usb_setup_packet_t* s;
  uint32_t t0, flags;
  esp_err_t e;
  /* An IN transfer's data stage has to be a whole number of packets. */
  uint16_t rounded = ((wantLen + EP0_MPS - 1) / EP0_MPS) * EP0_MPS;

  if (ctrlXfer == NULL &&
      usb_host_transfer_alloc(CTRL_BUF, 0, &ctrlXfer) != ESP_OK) {
    Serial.println("  transfer_alloc failed");
    return -1;
  }
  if (sizeof(usb_setup_packet_t) + rounded > CTRL_BUF) {
    Serial.println("  descriptor larger than our buffer");
    return -1;
  }

  s = (usb_setup_packet_t*)ctrlXfer->data_buffer;
  USB_SETUP_PACKET_INIT_GET_CONFIG_DESC(s, 0, wantLen);
  ctrlXfer->num_bytes = sizeof(usb_setup_packet_t) + rounded;
  ctrlXfer->device_handle = openDev;
  ctrlXfer->bEndpointAddress = 0;
  ctrlXfer->callback = ctrlCb;
  ctrlXfer->context = NULL;
  ctrlDone = false;

  e = usb_host_transfer_submit_control(client, ctrlXfer);
  if (e != ESP_OK) {
    Serial.printf("  submit_control: %s\n", esp_err_to_name(e));
    return -1;
  }
  t0 = millis();
  while (!ctrlDone && millis() - t0 < 1000) {
    usb_host_lib_handle_events(0, &flags);
    usb_host_client_handle_events(client, 1);
  }
  if (!ctrlDone) {
    Serial.println("  control transfer timed out");
    return -1;
  }
  if (ctrlStatus != USB_TRANSFER_STATUS_COMPLETED) {
    Serial.printf("  control transfer status %d\n", ctrlStatus);
    return -1;
  }
  return ctrlBytes - (int)sizeof(usb_setup_packet_t);
}

static void readConfigOurselves() {
  const uint8_t* d;
  uint16_t total;
  int n;

  Serial.println("\nasking the device directly over endpoint 0:");

  n = getDescriptor(9);
  if (n < 9) {
    Serial.printf("  short header (%d bytes) - endpoint 0 is not usable\n", n);
    return;
  }
  d = ctrlXfer->data_buffer + sizeof(usb_setup_packet_t);
  total = (uint16_t)(d[2] | ((uint16_t)d[3] << 8));
  Serial.printf("  config header ok: wTotalLength = %u, %u interfaces\n",
                total, d[4]);
  if (total > 256) {
    Serial.println("  ** larger than CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE");
    Serial.println("     (256), which is why the library's own enumeration");
    Serial.println("     never got this far.");
  }

  n = getDescriptor(total);
  if (n < (int)total) {
    Serial.printf("  full descriptor came back short (%d of %u)\n", n, total);
    return;
  }
  dumpConfig((const usb_config_desc_t*)(ctrlXfer->data_buffer +
                                        sizeof(usb_setup_packet_t)));
}

static void describe(uint8_t addr) {
  const usb_device_desc_t* dd = NULL;
  const usb_config_desc_t* cd = NULL;
  usb_device_info_t info;
  esp_err_t err;

  if (openDev == NULL) {
    if (usb_host_device_open(client, addr, &openDev) != ESP_OK) {
      Serial.printf("could not open device %u\n", addr);
      return;
    }
  }
  Serial.println("\n================ device found ================");

  if (usb_host_get_device_descriptor(openDev, &dd) == ESP_OK && dd != NULL) {
    Serial.printf("  %-14s %04X:%04X\n", "vid:pid", dd->idVendor, dd->idProduct);
    Serial.printf("  %-14s %02X/%02X/%02X\n", "device class", dd->bDeviceClass,
                  dd->bDeviceSubClass, dd->bDeviceProtocol);
    Serial.printf("  %-14s %X.%02X\n", "usb version", dd->bcdUSB >> 8,
                  dd->bcdUSB & 0xFF);
    Serial.printf("  %-14s %u\n", "ep0 max", dd->bMaxPacketSize0);
  }
  if (usb_host_device_info(openDev, &info) == ESP_OK) {
    printStr("manufacturer", info.str_desc_manufacturer);
    printStr("product", info.str_desc_product);
    printStr("serial", info.str_desc_serial_num);
    Serial.printf("  %-14s %s\n", "speed",
                  info.speed == USB_SPEED_LOW ? "low" :
                  info.speed == USB_SPEED_FULL ? "full" : "high");
  }
  /* This one can legitimately be absent: a device that has been addressed but
   * not yet configured has no active configuration to hand back. */
  err = usb_host_get_active_config_descriptor(openDev, &cd);
  if (err == ESP_OK && cd != NULL) {
    dumpConfig(cd);
  } else {
    Serial.printf("\nthe library has no active configuration (%s)\n",
                  cd == NULL ? "null descriptor" : esp_err_to_name(err));
    readConfigOurselves();
    usb_host_device_close(client, openDev);
    openDev = NULL;
  }
  Serial.println("=============================================\n");
}

/* ----------------------------------------------------------- host events */

/* Called during enumeration, once the device descriptor is in. Must not block
 * and must not submit transfers, so it only picks a configuration. */
static bool enumFilter(const usb_device_desc_t* dd, uint8_t* configValue) {
  *configValue = 1;
  (void)dd;
  return true;
}

static void clientCb(const usb_host_client_event_msg_t* msg, void* arg) {
  (void)arg;
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      pendingAddr = msg->new_dev.address;
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      Serial.println("device gone");
      if (openDev != NULL) {
        usb_host_device_close(client, openDev);
        openDev = NULL;
      }
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== DualSense32 USB probe ===");

  /* Do this first: once the host driver takes the pads they stop being GPIOs. */
  checkPins();
  Serial.println();

  usb_host_config_t hostCfg = {};
  hostCfg.skip_phy_setup = false;
  hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  /* This build has CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK on. Leaving
   * the callback null leaves the device addressed but never configured, which
   * is exactly the state we were stuck in. Say yes to everything, using
   * configuration 1. */
  hostCfg.enum_filter_cb = enumFilter;
  esp_err_t err = usb_host_install(&hostCfg);
  Serial.printf("usb_host_install: %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return;

  /* Harmless when it is already on, and rules the question out entirely. */
  err = usb_host_lib_set_root_port_power(true);
  Serial.printf("root_port_power:  %s\n", esp_err_to_name(err));

  usb_host_client_config_t clientCfg = {};
  clientCfg.is_synchronous = false;
  clientCfg.max_num_event_msg = 5;
  clientCfg.async.client_event_callback = clientCb;
  clientCfg.async.callback_arg = NULL;
  err = usb_host_client_register(&clientCfg, &client);
  Serial.printf("client_register:  %s\n", esp_err_to_name(err));

  Serial.println("\nwaiting for a device on the OTG port...");
}

void loop() {
  uint32_t flags = 0;
  usb_host_lib_handle_events(0, &flags);
  if (client != NULL) usb_host_client_handle_events(client, 10 / portTICK_PERIOD_MS);

  if (pendingAddr != 0) {
    uint8_t addr = pendingAddr;
    pendingAddr = 0;
    describe(addr);
  }

  /* The NEW_DEV callback is the normal path, but a client only hears about
   * devices enumerated after it registered — and a device already sitting on
   * the bus at startup produces no event at all. Ask the library directly
   * rather than waiting for a message that is never coming. */
  if (openDev == NULL && millis() - lastNag > 2000) {
    uint8_t addrs[8];
    int n = 0;
    lastNag = millis();
    esp_err_t e = usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n);
    if (e == ESP_OK && n > 0) {
      Serial.printf("(no event, but the library lists %d device%s - opening "
                    "address %u)\n", n, n == 1 ? "" : "s", addrs[0]);
      describe(addrs[0]);
      return;
    }
    dumpPort();
    Serial.printf("addr_list_fill: %s, listed %d\n", esp_err_to_name(e), n);

    /* The list can lag behind reality. Addresses are handed out from 1, so
     * just try to open the first few and let the error say what is wrong.
     * NOT_FOUND on all of them means enumeration genuinely never finished. */
    for (uint8_t a = 1; a <= 4; a++) {
      usb_device_handle_t h = NULL;
      esp_err_t r = usb_host_device_open(client, a, &h);
      Serial.printf("  open addr %u: %s\n", a, esp_err_to_name(r));
      if (r == ESP_OK) {
        openDev = h;          /* describe reuses the handle we already have */
        describe(a);
        break;
      }
    }
  }
}

#endif /* SOC_USB_OTG_SUPPORTED */

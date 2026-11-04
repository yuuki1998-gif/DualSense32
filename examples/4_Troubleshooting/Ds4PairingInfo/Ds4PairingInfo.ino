/*
 * Ds4PairingInfo — which host is this DualShock 4 still looking for?
 *
 * A DualShock 4 remembers one host address and pages it when PS is pressed.
 * A PS4 writes its own address there over USB the moment the pad is plugged
 * in; SixaxisPairTool writes it by hand. Either way, a pad that is quietly
 * calling an address that is not there looks exactly like a pad that is
 * broken — it lights up, and nothing happens.
 *
 * Feature report 0x12 holds it, and USB can read it without pairing first.
 * The whole 16 bytes are dumped rather than only the fields this sketch
 * claims to understand, because only the first six are confirmed here: the
 * library already relies on those for the controller's own address. Compare
 * what comes out against the address you last wrote with SixaxisPairTool.
 *
 * Plug the pad into the native USB port. No dongle, no pairing needed.
 */

#include <ds_usb_hid.h>
#include <ds_protocol.h>

static void hexmac(const uint8_t *p) {
  /* Bluetooth addresses are written the reverse of their wire order. */
  for (int i = 5; i >= 0; i--) Serial.printf("%02X%s", p[i], i ? ":" : "");
}

static void onAttach(void *ctx, uint16_t vid, uint16_t pid) {
  (void)ctx;
  Serial.printf("\nattached VID %04X PID %04X", vid, pid);
  bool ds4 = (pid == DS_USB_PID_DUALSHOCK4_V1 ||
              pid == DS_USB_PID_DUALSHOCK4_V2 ||
              pid == DS_USB_PID_DUALSHOCK4_DONGLE);
  Serial.printf("  (%s)\n", ds4 ? "DualShock 4" : "not a DualShock 4");
  if (!ds4) return;

  uint8_t f[32];
  int n = ds_usb_hid_get_feature(DS4_FEATURE_PAIRING_INFO, f,
                                  DS4_FEATURE_PAIRING_INFO_SIZE, 800);
  Serial.printf("feature 0x12 -> %d bytes\n", n);
  if (n <= 0) {
    Serial.println("  no answer; the pad did not offer it");
    return;
  }

  Serial.print("  raw  ");
  for (int i = 0; i < n; i++) Serial.printf("%02X ", f[i]);
  Serial.println();

  /* Confirmed: the library reads the controller's own address from here. */
  if (n >= 7) {
    uint8_t self[6];
    if (ds_parse_pairing_info_dev(f, (uint16_t)n, DS_DEVICE_DUALSHOCK4, self)) {
      Serial.print("  this controller : ");
      hexmac(self);
      Serial.println();
    }
  }

  /* Reported to be the host address, and the reason for this sketch. Printed
   * as a candidate rather than a fact — check it against what you wrote. */
  if (n >= 16) {
    Serial.print("  host it will page: ");
    hexmac(f + 10);
    Serial.println("   <- compare with SixaxisPairTool");
    bool blank = true;
    for (int i = 10; i < 16; i++) if (f[i] != 0x00 && f[i] != 0xFF) blank = false;
    if (blank) Serial.println("  (looks unset — the pad has no host to call)");
  }
  Serial.println();
}

static void onDetach(void *ctx) { (void)ctx; Serial.println("unplugged"); }

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("Ds4PairingInfo: plug the pad into the native USB port");
  ds_usb_hid_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.on_attach = onAttach;
  cfg.on_detach = onDetach;
  if (!ds_usb_hid_init(&cfg)) Serial.println("usb init failed");
}

void loop() { ds_usb_hid_poll(); delay(5); }

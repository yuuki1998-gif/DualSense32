/*
 * DualShock4 (dongle) — a PS4 pad over Bluetooth, and proof it was read as one.
 *
 * There is no USB product id out here. The only thing that names the pad is
 * the input report id it sends: 0x31 is a DualSense, 0x11 a DualShock 4. Both
 * pads send 0x01 until a feature report is asked for, and that one identifies
 * nothing — so this sketch prints the id it settled on, not just the answer.
 *
 * It also prints the calibration. That is the part with a trap: over Bluetooth
 * both pads keep it behind feature report 0x05, at the same length, and it
 * arrives *before* the first full report. So the link stores the bytes and
 * reads them again once the pad has named itself. A DualShock 4 groups its
 * three gyro plus readings ahead of the minus ones where a DualSense pairs
 * them per axis; read the wrong way round it scales pitch by roll's numbers,
 * silently. Sane denominators here mean the second read happened.
 *
 * Hardware: ESP32-S3 or S2, Bluetooth dongle in the native USB host port.
 * Log on the UART port. Nothing else should be in the native port.
 *
 * First run: on the pad hold SHARE, then PS, until the light bar flashes fast.
 *   (SHARE is the small button left of the touchpad — CREATE on a DualSense.)
 * Flash with Tools > Erase All Flash Before Sketch Upload = Enabled the first
 * time, or a stale link key on either side fails authentication for ever.
 * After that: press PS alone.
 */

#include <DualShock4.h>
#include <ds_link_usb_hci.h>

DualShock4 pad;

/* The HCI conversation and the channel handshake narrate themselves here, and
 * a link that will not come up says why in this and nowhere else. */
static void onLog(void *user, const char *msg) {
  (void)user;
  Serial.printf("[%7.2fs] %s\n", millis() / 1000.0f, msg);
}

static bool announced = false;
static uint8_t reportId = 0;

static void announce() {
  Serial.println();
  Serial.println("-------------------------------------------------");
  Serial.printf("connected, and it is a %s\n",
                pad.isDualShock4() ? "DualShock 4" : "DualSense");
  Serial.printf("  named by input report id 0x%02X\n", reportId);

  char mac[18];
  if (pad.address(mac)) Serial.printf("  address %s\n", mac);
  Serial.printf("  battery %u/10%s\n", pad.battery(),
                pad.charging() ? ", charging" : "");

  /* The two pads differ in what hardware exists at all. */
  Serial.printf("  adaptive triggers %s, mic %s, player leds %s, self-blink %s\n",
                pad.supports(DS_CAP_ADAPTIVE_TRIGGERS) ? "yes" : "no",
                pad.supports(DS_CAP_MIC_LED) ? "yes" : "no",
                pad.supports(DS_CAP_PLAYER_LEDS) ? "yes" : "no",
                pad.supports(DS_CAP_LIGHTBAR_BLINK) ? "yes" : "no");

  /* Declaring DualShock4 above is what makes the missing hardware a build
   * error rather than a value nobody checks. None of these compile:
   *
   *   uint8_t block[DS5_TRIGGER_BLOCK_SIZE];
   *   ds5_trigger_weapon(block, 3, 6, 8);
   *   pad.triggerRight(block);   error: use of deleted function
   *   pad.micLed(2);             error: use of deleted function
   *   pad.playerLeds(10101);     error: use of deleted function
   *   pad.down(DS4_MUTE);        error: 'DS4_MUTE' was not declared
   *
   * Through DualSense32 they would return false while the sketch runs, which
   * only helps if somebody checks the return. */
  Serial.println("-------------------------------------------------");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("dongle in the native USB port; SHARE + PS on the pad");
  ds_link_usb_hci_on_log(pad.transport(), onLog, NULL);
  if (!pad.begin()) Serial.println("begin failed");
}

/* The HCI layer narrates failures but not the inquiry, so watch the state
 * machine directly: idle -> inquiring -> ... is the only way to tell a scan
 * that found nothing from one that never ran. */
static void watchHost() {
  ds_hci_host_t *h = ds_link_usb_hci_host(pad.transport());
  if (h == NULL) return;

  static int8_t lastState = -1;
  if ((int8_t)h->state != lastState) {
    lastState = (int8_t)h->state;
    Serial.printf("[%7.2fs] state: %s\n", millis() / 1000.0f,
                  ds_hci_host_state_name(h->state));
  }

  /* The dongle's own address, which is what a pad has to be told to page. */
  static bool saidAddr = false;
  if (!saidAddr) {
    const uint8_t *a = h->local_addr;
    if (a[0] || a[1] || a[2] || a[3] || a[4] || a[5]) {
      saidAddr = true;
      Serial.printf("[%7.2fs] dongle address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    millis() / 1000.0f, a[5], a[4], a[3], a[2], a[1], a[0]);
    }
  }
}

void loop() {
  pad.update();
  watchHost();

  /* A stored bond switches the scan off deliberately: a pad that already
   * knows us pages in by itself, and scanning while it tries would stop it
   * being answered. The cost is that a *different* pad can never be found
   * while the old bond survives, and from the outside that is independent
   * from a controller that simply will not pair. Send 'c' to forget it. */
  if (Serial.available() && Serial.read() == 'c') {
    ds_link_usb_hci_clear_bond(pad.transport());
    Serial.println();
    Serial.println("bond forgotten - restarting, will scan for a new pad");
    delay(200);
    ESP.restart();
  }

  /* A stored bond turns the scan off on purpose: a pad that already knows us
   * pages in by itself, and scanning while it does that stops it answering.
   * The cost is that a *different* pad can never be found while the old bond
   * survives — which looks exactly like a controller that will not pair.
   * Send 'c' to forget it, then reset. */
  if (Serial.available() && Serial.read() == 'c') {
    ds_link_usb_hci_clear_bond(pad.transport());
    Serial.println("bond forgotten — resetting to scan for a new pad");
    delay(200);
    ESP.restart();
  }

  if (!pad.connected()) {
    announced = false;
    reportId = 0;
    return;
  }

  /* Remember the first id that actually named the pad, so the line above is
   * evidence rather than a restatement of what the library decided. */
  uint8_t id = pad.raw().report_id;
  ds_device_t named;
  if (reportId == 0 && ds_identify_bt_report(id, &named)) reportId = id;

  if (!announced && reportId != 0) { announce(); announced = true; }
  if (!announced) return;

  /* The light bar and the motors are on both pads. */
  if (pad.pressed(DS_CROSS))    pad.lightbar(0, 0, 255);
  if (pad.pressed(DS_CIRCLE))   pad.lightbar(255, 0, 0);
  if (pad.pressed(DS_TRIANGLE)) pad.lightbar(0, 255, 0);
  /* Each trigger drives the motor on its own side: rumble() takes left
   * first. A small deadzone, because a worn trigger does not always come all
   * the way back and a motor running on its own drowns out everything else. */
  uint8_t l2 = pad.l2() > 12 ? pad.l2() : 0;
  uint8_t r2 = pad.r2() > 12 ? pad.r2() : 0;
  pad.rumble(l2, r2);

  pad.printEvents(Serial);

  /* Motion a couple of times a second. If the calibration was read the wrong
   * way round these are wrong by a factor of sixty, or wrong per axis. */
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  if (pad.raw().has_motion) {
    Serial.printf("gyro %7.1f %7.1f %7.1f deg/s   accel %5.2f %5.2f %5.2f g\n",
                  pad.gyroPitch(), pad.gyroYaw(), pad.gyroRoll(),
                  pad.accelX(), pad.accelY(), pad.accelZ());
  }
}

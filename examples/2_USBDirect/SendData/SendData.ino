#include <ds_link_usb_hid.h>
#include <DualSense32.h>

/*
 * SendData -- everything the host can drive, and a way to feel the difference.
 *
 * Every adaptive trigger effect the controller has, one button each:
 *
 *   d-pad left     off                d-pad up      resistance
 *   d-pad right    weapon             d-pad down    vibration
 *   square         slope              triangle      bow
 *   circle         galloping          cross         machine
 *   L1             notched resistance R1            stepped vibration
 *
 *   L2 and R2      rumble, harder the further you pull
 *   Create         walk the player lamps along
 *   Mute           cycle the mic lamp
 *
 * One press, one effect: nothing cycles and there is no mode to remember.
 * Both triggers get the same effect so one can be pulled against the other,
 * and the light bar colour says which is loaded. Left puts it all back.
 *
 * Bow, galloping and machine are undocumented: worked out by the community
 * rather than published by Sony.
 *
 * Hardware: an ESP32-S3 (or S2) with the pad on a USB-A to USB-C cable.
 *
 * No radio, no dongle, no pairing: the pad is a USB HID device on a wire and
 * this board is the host. Report ids, field order and framing all differ from
 * Bluetooth, and the library carries that difference so this sketch does not.
 */

DualSense32 pad(ds_link_usb_hid());

/*
 * Positions below are the trigger's own ten zones: 0 is released, 9 is
 * bottomed out. Each mode carries the colour the light bar shows while it is
 * loaded and the button that selects it; Off is the resting colour, so the
 * left arrow puts everything back.
 */
struct Mode {
  const char* name;
  uint8_t r, g, b;
  DSButton key;   /* the shared names: every one of these is on both pads */
};

static const Mode MODES[] = {
  /* name                  r    g    b   button        */
  {"off",                0, 160, 255,   DS_LEFT},
  {"resistance",         255, 140,   0, DS_UP},
  {"weapon",             255,   0,  40, DS_RIGHT},
  {"vibration",          160,   0, 255, DS_DOWN},
  {"slope",              255, 230,   0, DS_SQUARE},
  {"bow",                  0, 255, 100, DS_TRIANGLE},
  {"galloping",          255,   0, 200, DS_CIRCLE},
  {"machine",            255, 255, 255, DS_CROSS},
  {"notched resistance",   0,  60, 255, DS_L1},
  {"stepped vibration",  120, 255,   0, DS_R1},
};
#define MODE_COUNT (sizeof(MODES) / sizeof(MODES[0]))

static void loadMode(uint8_t which) {
  uint8_t block[DS5_TRIGGER_BLOCK_SIZE];

  /* Three separated notches rather than a ramp: this is what per-zone
   * control buys you, and a smooth slope would not show it. */
  static const uint8_t NOTCHES[10] = {0, 0, 8, 0, 0, 8, 0, 0, 8, 0};
  /* Buzzes that get stronger the further in you go. */
  static const uint8_t STEPS[10] = {0, 2, 0, 4, 0, 6, 0, 8, 0, 0};

  switch (which) {
    case 1: ds5_trigger_feedback(block, 3, 8); break;
    case 2: ds5_trigger_weapon(block, 4, 6, 8); break;
    case 3: ds5_trigger_vibration(block, 2, 6, 40); break;
    case 4: ds5_trigger_slope(block, 1, 8, 2, 8); break;
    case 5: ds5_trigger_bow(block, 1, 6, 6, 8); break;
    case 6: ds5_trigger_galloping(block, 1, 9, 2, 5, 15); break;
    case 7: ds5_trigger_machine(block, 1, 9, 3, 7, 20, 5); break;
    case 8: ds5_trigger_multi_feedback(block, NOTCHES); break;
    case 9: ds5_trigger_multi_vibration(block, 30, STEPS); break;
    default: ds5_trigger_off(block); break;
  }

  /* Both, so there is something to compare against. */
  pad.triggerLeft(block);
  pad.triggerRight(block);
  pad.lightbar(MODES[which].r, MODES[which].g, MODES[which].b);
  Serial.printf("mode: %s\n", MODES[which].name);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nSendData - d-pad, face buttons and bumpers pick an effect");

  if (!pad.begin()) Serial.println("begin failed");

  pad.playerLeds(1);
}

void loop() {
  /* Kept across frames: the mode, the lamps, and whether the pad has been
   * told about them yet. */
  static bool armed = false;
  static uint8_t mode = 0;

  pad.update();

  /* A pad that goes away should be told everything again when it returns. */
  if (!pad.connected()) {
    armed = false;
    return;
  }

  if (!armed) {
    armed = true;
    loadMode(mode);
  }

  for (uint8_t i = 0; i < MODE_COUNT; i++) {
    if (pad.pressed(MODES[i].key) && i != mode) {
      mode = i;
      loadMode(mode);
    }
  }

  /* Five lamps in the low five bits. Walking one lit lamp along them is the
   * clearest way to see they are addressed individually. */
  static uint8_t lamps = 0x01;
  if (pad.pressed(DS_CREATE)) {
    lamps = (uint8_t)(lamps << 1);
    if (lamps > 0x10) lamps = 0x01;
    pad.playerLedsMask(lamps);
  }

  static uint8_t mic = 0;
  if (pad.pressed(DS5_MUTE)) {
    mic = (uint8_t)((mic + 1) % 3);   /* off, solid, pulsing */
    pad.micLed(mic);
    Serial.printf("mic lamp: %u\n", mic);
  }

  /* Two motors: the left one is the low, heavy rumble and the right the
   * quicker one. Driving each from its own trigger makes that obvious. */
  pad.rumble(pad.l2(), pad.r2());

  /*
   * Nothing is sent from here. Every setter above marks the output as
   * changed and update() carries it out, twenty times a second by default.
   * pad.send() is still there for a sketch that wants to pick the moment,
   * and pad.sendInterval() for one that wants a different rate.
   */
}

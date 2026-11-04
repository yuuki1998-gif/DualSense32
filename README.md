# DualSense32

[English](README.md) | [日本語](README.ja.md)

A PlayStation 5 DualSense or a PlayStation 4 DualShock 4 on an ESP32.
Buttons, sticks, triggers, touchpad, motion and battery come back; light bar
colour and rumble go out, and on a DualSense the mic LED and all ten adaptive
trigger effects as well.

**It works wirelessly on the ESP32-S3 and S2, which have no Bluetooth Classic
radio**, by driving a standard USB Bluetooth dongle directly and running the
host stack — HCI, pairing, L2CAP, SDP — in this library.

Either pad runs on any of the three links. Which one is on the other end is
worked out by the link rather than guessed, and a call the connected pad
cannot honour is refused where you can see it rather than dropped in silence.

> **Manual** — from unzipping to the first sketch running, with figures and
> worked examples.
> - English: [docs/MANUAL-en.md](docs/MANUAL-en.md), or open
>   [docs/DualSense32-manual-en.html](docs/DualSense32-manual-en.html) /
>   `docs/DualSense32-manual-en.pdf` locally
> - Japanese: [docs/MANUAL-ja.md](docs/MANUAL-ja.md) — `取扱説明書を開く.html`
>   and `取扱説明書.pdf` (A4, 31pp) ship at the repository root

---

## 1. Choosing a link

Three ways to reach the pad. **Your board decides which.**

| | Board | Also needs | |
|---|---|---|---|
| **1_Dongle** | ESP32-S3 / S2 | a Bluetooth dongle (USB class `E0/01/01`) + a USB-C to USB-A adapter | wireless, on a radio separate from the ESP32's own WiFi |
| **2_USBDirect** | ESP32-S3 / S2 | a USB-C cable (C to C) | wired, no pairing |
| **3_BTClassic** | original ESP32 only | nothing | wireless, nothing to buy |

The ESP32-S3, C3, C6 and H2 **have no Bluetooth Classic radio** — BLE only —
and both pads speak Classic. So wireless on an S3 means an external dongle.
The original ESP32 is the other way round: it has Classic but no USB host, so
a dongle is not an option there.

### What makes a dongle usable

| | |
|---|---|
| USB class | `E0/01/01`. That is what marks a standard HCI dongle |
| Radio | Bluetooth Classic (BR/EDR). **BLE-only will not work** |
| Firmware | on the dongle. Ones that stay inert until a host driver uploads firmware are out of scope |
| Power / speed | bus powered, ~100 mA, full speed |
| Plug | USB-A, so an S3 needs an adapter |

The Bluetooth 5.x / 6.0 on the box is irrelevant; only avoid the BLE-only ones.

Developed against a **UGREEN Bluetooth 6.0 adapter** (Windows Device Manager
calls it `BARROT Bluetooth 6.0 Adapter`). What the ESP32-S3 saw:
`33FA:0012`, class `E0/01/01`, product `UGREEN BT6.0 Adapter`, full speed —
and it needed no firmware upload.

**The dongle goes in the S3's native USB port**, not the UART one; leave that
plugged into the PC for flashing, the serial monitor and power. If you cannot
tell the two ports apart on your board, flash
`4_Troubleshooting/WhatIsOnUSB` and move the dongle between them.

---

## 2. Installing

Unzip the release (`DualSense32-1.7.0.zip`) and move the whole `DualSense32`
folder into the IDE's library directory:

| OS | Path |
|---|---|
| Windows | `Documents\Arduino\libraries\DualSense32\` |
| macOS | `~/Documents/Arduino/libraries/DualSense32/` |
| Linux | `~/Arduino/libraries/DualSense32/` |

`library.properties` has to end up directly inside `DualSense32/`. Handing the
ZIP to **Sketch > Include Library > Add .ZIP Library** works too. Restart the
IDE and the examples appear under **File > Examples > DualSense32**.

From git instead:

```
cd ~/Arduino/libraries          # Documents\Arduino\libraries on Windows
git clone https://github.com/yuuki1998-gif/DualSense32
```

Built and tested against **Arduino ESP32 core 3.3.11**.

### Board settings, in the Tools menu

| Route | Setting |
|---|---|
| original ESP32 (3_BTClassic) | **Partition Scheme = Huge APP (3MB No OTA/1MB SPIFFS)**. On the default the sketch does not fit: `text section exceeds available space` |
| ESP32-S3 (1_Dongle / 2_USBDirect) | **USB CDC On Boot = Enabled**, or the serial monitor stays silent |
| first wireless connection | **Erase All Flash Before Sketch Upload = Enabled**. A stale link key on either side fails authentication for ever |

---

## 3. The first sketch

This is **File > Examples > DualSense32 > (your route) > Basics**.

```cpp
#include <DualSense32.h>

DualSense32 pad;

void setup() {
  Serial.begin(115200);
  if (!pad.begin()) Serial.println("begin failed");
}

void loop() {
  pad.update();                        // once at the top, every time
  if (!pad.connected()) return;

  if (pad.pressed(DS_CROSS)) Serial.println("cross down");

  if (pad.down(DS_CROSS)) {
    pad.lightbar(255, 0, 0);
    pad.rumble(120, 120);
  } else {
    pad.lightbar(0, 160, 255);
    pad.rumble(0, 0);
  }
}
```

**The three routes differ by the first line.** Nothing below it changes.

```cpp
DualSense32 pad;                       // automatic: the built-in radio on an
                                       // original ESP32, a dongle on an S3/S2
DualSense32 pad(ds_link_usb_hid());    // pad on a cable  + #include <ds_link_usb_hid.h>
```

**No argument is the ordinary form**, and `#include <DualSense32.h>` is then the
only include needed. An original ESP32 has a Bluetooth Classic radio and no USB
host controller; an S3 or S2 is the other way round. Once the chip is known,
there is only one way to reach a pad wirelessly, and the library takes it.

**Only the cable needs saying out loud.** On an S3 a dongle and a pad go into
the same USB port, and nothing at compile time distinguishes them; the dongle is
the default.

Naming a link explicitly always works:

```cpp
DualSense32 pad(ds_bt_bluedroid());    // original ESP32  + #include <ds_bt_bluedroid.h>
DualSense32 pad(ds_link_usb_hci());    // dongle          + #include <ds_link_usb_hci.h>
```

`update()` is not optional and its absence is silent: no button responds and
the light bar never changes colour. Reports arrive on the link's own task, so
reading them straight would give you a stick from this frame and a button from
the last. `update()` takes one consistent copy and sends whatever outputs you
set — you do not call `send()` yourself.

**Pairing**, once: hold **CREATE** — **SHARE** on a DualShock 4 — then press
**PS**, and let go when the light bar flashes quickly. After that, the PS
button alone. Either pad remembers exactly one host.

### Which pad

A DualShock 4 has no adaptive triggers, no microphone or mute button and no
player LEDs. It has one thing a DualSense has not: a light bar it blinks by
itself, `lightbarBlink(on, off)`, in units of 10 ms. Its touchpad is
1920 x 942 where a DualSense is 1920 x 1080. Two ways to write for that.

`DualSense32` takes whichever pad turns up and answers for it at run time, so
one binary serves both:

```cpp
if (pad.supports(DS_CAP_ADAPTIVE_TRIGGERS)) pad.triggerRight(block);
```

The setters that reach for optional hardware — `triggerLeft`, `triggerRight`,
`micLed`, `playerLeds`, `playerLedsMask`, `lightbarBlink` — return `bool`, and
return false when the connected pad cannot do it. The refusals are counted and
`printEvents()` reports each one once, because a field dropped while the
report is built looks exactly like one that arrived.

Naming the pad in the type instead moves the check to the compiler:

```cpp
#include <DualShock4.h>

DualShock4 pad(ds_link_usb_hid());
```

The hardware it does not have is `= delete`d, so `pad.triggerRight(b)`,
`pad.micLed(2)` and `pad.playerLeds(...)` are build errors that name the call.
Buttons normally use the shared `DSButton` names: `DS_CROSS`, `DS_CREATE`, and so on. `DS4Button` aliases such as `DS4_SHARE` are kept when you want Sony's printed names. There is no
`DS4_MUTE` at all, so reaching for the mute button is `error: 'DS4_MUTE' was
not declared in this scope`. Writing a DualSense button alias such as `DS5_CROSS` on a `DualShock4` is a build error too. Use the shared `DS_CROSS` spelling for buttons both pads have. `begin()` on this type starts having already said which pad to expect, so
there is nothing extra to write, and the address form is unchanged:

```cpp
pad.begin();                        // whichever DualShock 4 turns up
pad.begin("AA:BB:CC:DD:EE:FF");     // that one
```

On a plain `DualSense32` the expectation is an argument instead:

```cpp
DualSense32 pad(ds_bt_bluedroid());
pad.begin(DS_DEVICE_DUALSHOCK4);
pad.begin(DS_DEVICE_DUALSHOCK4, "AA:BB:CC:DD:EE:FF");
pad.begin();                        // as before: take whatever connects
```

**It is an expectation, not an override.** Over Bluetooth the pad does not
name itself until its first full report, and until then there is no way to
know which *output* report to build. Saying so up front makes output correct
from the first frame; not saying so holds it — the setting is kept and
delivered once the pad speaks, rather than built wrong and thrown away.

Whatever the link works out for itself always wins. If the pad turns out to be
the other one, `device()` says so and `mismatched()` reports it. To override a
product id — for a clone that reports someone else's —
`ds_link_usb_hid_force_device()` is the tool for that instead.

Input does not depend on the expectation: the report that arrives during that
window is `0x01`, whose layout is identical on both pads, which is also why
the library refuses to guess the model from it.

`connected()` is true only once a DualShock 4 really is on the
other end; `mismatched()` is true when something else is.

---

## 4. Quick reference

```cpp
pad.down(DS_CROSS)          // held
pad.pressed(DS_CROSS)       // the instant it went down
pad.released(DS_CROSS)      // the instant it came up

pad.leftX()  pad.leftY()     // -128..127, deadzone applied, +Y is down
pad.rightX() pad.rightY()
pad.l2()     pad.r2()        // 0..255
pad.battery()                // 0..10
pad.raw().touch[0].x         // touchpad, 0..1919 by 0..1079 (0..941 on a DS4)
pad.gyroYaw()  pad.accelZ()  // deg/s and g, already divided

pad.supports(DS_CAP_ADAPTIVE_TRIGGERS)   // what this pad can do
pad.isDualShock4()                       // which pad it is

pad.lightbar(r, g, b);       // 0..255 each
pad.rumble(left, right);     // 0..255 each
pad.lightbarBlink(on, off);  // DualShock 4 only, units of 10 ms
pad.playerLeds(10101);       // write the pattern you want lit
pad.micLed(1);               // 0 off / 1 on / 2 blink

uint8_t b[DS5_TRIGGER_BLOCK_SIZE];
ds5_trigger_weapon(b, 3, 6, 8);        // one of ten effects, DualSense only
pad.triggerRight(b);

pad.deadzone(8);             // stick slack
pad.address(buf);            // who is connected
pad.printEvents(Serial);     // one line per change
```

Button names, the touchpad coordinate system and the arguments to all ten
trigger effects are in chapters 6 and 7 of the manual.

---

## 5. Examples

```
1_Dongle/         ESP32-S3 / S2 with a dongle
2_USBDirect/      ESP32-S3 / S2 with the pad on a cable
3_BTClassic/      original ESP32, its own radio
    Basics/       the shortest sketch that proves the link works
    ReceiveData/  everything the pad reports, as it happens
    SendData/     everything the host can drive
    DualShock4/   a PS4 pad, and where it differs (all three routes)
    DualShock4Only/  the same, with the pad named in the type (2_USBDirect)
    EitherPad/    one binary, either pad, branching on supports() (2_USBDirect)
    Ds4Lightbar/  the light bar a DS4 blinks itself, as a status light (2_USBDirect)
    Ds4Touchpad/  the touchpad actually used: two fingers, drags (2_USBDirect)
4_Troubleshooting/  DongleStepByStep / WhatIsOnUSB / EventLog / Ds4PairingInfo
5_SelfTest/         no pad, no dongle, no hardware at all
```

The sketches under each route differ only in that constructor argument.
`2_USBDirect/FeatureReports` and `4_Troubleshooting/Ds4PairingInfo` are the
exceptions: reading the pad's own feature reports is something only the cable
can do.

---

## 6. When it does not work

| Symptom | Cause | Fix |
|---|---|---|
| `text section exceeds available space` | default partition scheme | **Huge APP** (original ESP32) |
| nothing in the serial monitor | USB CDC On Boot disabled | enable it (S3) |
| pairing mode, still no connection | the ESP32 is holding an old key | reflash with **Erase All Flash**, and CREATE + PS on the pad |
| connects, then drops immediately | the two link keys disagree | same, and make **both** ends forget |
| a different pad is never found, however long it scans | a stored bond turns the inquiry scan off on purpose: a pad that already knows the host pages in by itself, and scanning would stop it being answered | `ds_link_usb_hci_clear_bond()`, then reset |
| adaptive triggers or the mic LED do nothing | the pad is a DualShock 4 and has neither | ask `pad.supports(...)` first; `printEvents()` names the refusal |
| one press fires dozens of times | `down()` where `pressed()` was meant | `pressed()` |
| light bar and rumble do nothing | `update()` is not being called | `pad.update()` at the top of `loop()` |
| it moves with nobody touching it | deadzone too small | `pad.deadzone(12)` |

The rest of the table is chapter 9 of the manual; the diagnostic sketches are
under `4_Troubleshooting/`.

---

## 7. Inside

Each layer stands alone. Use the top, or reach into the middle.

```
    your sketch
        |
    ds_transport_t              one seam, three links
        |
   +----+--------------------+---------------------+
   |                         |                     |
ds_bt_bluedroid      ds_link_usb_hci      ds_link_usb_hid
 (original ESP32)       ds_l2cap  ds_sdp   (pad on a wire)
        |               ds_hci_host              |
        |               ds_hci                   |
        |               ds_usb_hci           ds_usb_hid
        |                    |                    |
        +--------------------+--------------------+
                             |
                       ds_protocol      reports in and out
```

There is no third-party Bluetooth stack underneath: `ds_link_usb_hci` drives
the dongle's host controller interface directly, and `ds_l2cap` and `ds_sdp`
are the two protocols above it that a DualSense insists on.

`ds_protocol`, `ds_hci`, `ds_hci_host`, `ds_l2cap` and `ds_sdp` depend on
nothing but `string.h`. They do not block, do not allocate and do not read a
clock, which is why the whole connect-and-pair conversation can be replayed on
a desk with no hardware attached.

Which controller is on the other end is decided by the link and passed down,
never guessed from the report. Over USB it comes from the USB product id —
both pads answer to input report 0x01 at 64 bytes, so the report alone cannot
separate them. Over Bluetooth there is no product id, so the input report id
decides: 0x31 is a DualSense, 0x11 a DualShock 4. Report 0x01 identifies
nothing, since both pads send it until a feature report is asked for, and it
is deliberately not used to guess. The product ids recognised are 0x05C4
(CUH-ZCT1), 0x09CC (CUH-ZCT2) and 0x0BA0 (the USB adaptor);
`ds_link_usb_hid_force_device()` overrides them for a clone.

The C API is the library; the C++ class is a few hundred lines on top of it.
Reach for the C side when you want the transport seam itself — a link of your
own, the HCI conversation, L2CAP — or when C++ is not on the table.

### Verified

| Test | Assertions | Hardware |
|---|---|---|
| `5_SelfTest/ProtocolCore` | 57 | no |
| `5_SelfTest/Ds4Core` | 162 | no |
| `5_SelfTest/HciPackets` | 133 | no |
| `5_SelfTest/HciHost` | 317 | no |
| `5_SelfTest/L2cap` | 177 | no |
| | **846, all passing** | |

The expected bytes are transcribed from the primary sources — Linux's
`hid-playstation.c`, the specifications — and not from this implementation, so
a transcription slip fails a test instead of being baked into both sides. On
real hardware: an ESP32-S3-DevKitC-1 with a dongle, about 390 reports a second
with no failed sends. [VERIFICATION.md](VERIFICATION.md) records what was
measured and how.

The DualShock 4 has been run on an ESP32-S3 both ways, USB-direct and through
the dongle, with a CUH-ZCT2. Accelerometer magnitude came out 1.001 g and a
resting gyro within half a degree per second, neither of which holds unless
the DualShock 4 offsets were used and the calibration was read again once the
pad had named itself. The original-ESP32 Bluedroid route has the code but has
not been confirmed on hardware.

### Things that will bite you

**The gyro changes units when you calibrate it.** Raw is roughly 16 LSB per
deg/s and varies from pad to pad; calibrated it is exactly 1024. The
accelerometer is 8192 LSB per g either way, and that asymmetry is what makes
the mistake easy. `pad.gyroYaw()` and `pad.accelZ()` have already divided.

**The Bluetooth output sequence tag is inside the CRC.** Set
`ds_output_t::sequence` before building; stamping it into a finished buffer
invalidates the signature and the pad ignores every frame after the first.

**Everything on the USB links is polled.** A plain `delay()` in your loop
stops the pipes, and on the dongle route that means it stops Bluetooth. Pump
`tr->poll(tr)` while you wait.

---

## Licence

Copyright 2026 yuuki1998 — [PolyForm Noncommercial License 1.0.0](LICENSE)

**Noncommercial use only.** Personal study, hobby and research projects, and
schools and other noncommercial organisations (a school robot club entering a
student contest, for example) may use, modify and share it. Commercial use is
not covered by this licence; ask about a separate licence if you need one.
When you pass it on, include [LICENSE](LICENSE), [NOTICE](NOTICE) and
[LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt).

The headers under `src/stack/` and `src/osi/` are the exception: excerpts of
Espressif's Bluedroid internals, Copyright Broadcom Corporation and Google
Inc., which stay under [Apache-2.0](LICENSES/Apache-2.0.txt) and keep their
own notices. Only `ds_bt_bluedroid.c`,
the original-ESP32 link, needs them; the dongle and wired routes do not.

Written from the Bluetooth Core Specification (Vol 3 Part A for L2CAP, Vol 4
Parts B and E for HCI), the USB HID specification, and Linux's
`drivers/hid/hid-playstation.c` for the report layouts. Trigger effect packing
follows the format described by Nielk1's TriggerEffectGenerator, implemented
independently here. Where sources disagreed, hardware decided.

DualSense, DualShock and PlayStation are trademarks of Sony Interactive
Entertainment Inc. This project is not affiliated with, authorised by, or
endorsed by Sony.

# Verification

Every offset and constant in this library is either derived from a primary
source or measured on real hardware. This file records which, so nothing rests
on "it seemed to work".

## Sources

| Source | Used for |
|---|---|
| [linux/drivers/hid/hid-playstation.c](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c) | report IDs, input/output struct layouts, CRC seeds, status bit fields |
| [nondebug/dualsense](https://github.com/nondebug/dualsense) | the 10-byte Bluetooth compatibility report 0x01 |
| [HamzaYslmn/esp-ps5](https://github.com/HamzaYslmn/esp-ps5) | an independent write-up of the same wire offsets, used as a cross-check |
| [Nielk1 TriggerEffectGenerator](https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db) | adaptive trigger bit packing |

Where two sources disagreed, hardware decided.

## Offsets

`ds_protocol.c` carries the output-report offsets as an enum and the input
layouts as a table. Both were re-derived independently by walking the Linux
struct definitions field by field and comparing against the C:

```
CO_VALID_FLAG0 .. CO_LIGHTBAR_BLUE   15 constants   all match
LAYOUT_FULL gyro/accel/timestamp/touch/status        all match
output common size 47  -> USB report 0x02 = 63 bytes  matches
input  common size 63  -> BT  report 0x31 = 78 bytes  matches
```

## Self-test

`examples/SelfTest/ProtocolCore` runs 57 assertions with no controller attached. The
expected byte strings in `vectors.h` are generated from the reference sources —
Linux structs, Nielk1's packing, Python's `zlib.crc32` — **not** from this
implementation, so a transcription slip fails the test rather than being
baked into both sides.

| Board | Result |
|---|---|
| ESP32 (original), Arduino-ESP32 3.3.11 | 53/53 PASS (before the sequence tests were added) |
| ESP32-S3 (LOLIN S3 Mini), Arduino-ESP32 3.3.11 | **57/57 PASS** |

Coverage: CRC-32 against known values and chaining, all four trigger effects
plus their out-of-range fallbacks, Bluetooth and USB output framing, the
sequence tag, a real captured 0x31 frame, a compatibility 0x01 frame, and the
rejection paths (unknown report id, truncated frame, wrong link, short buffer).

## Measured on hardware

These were confirmed with a DualSense (BT MAC `24:A6:FA:xx:xx:xx`, the
last three octets redacted throughout) on an
original ESP32, using the predecessor library that this core was extracted
from.

| Claim | Evidence |
|---|---|
| Output report 0x31 + CRC32 is accepted | sending one makes the pad switch its input from 0x01 to 0x31 by itself; a wrong CRC is dropped silently and it would stay on 0x01 |
| Lightbar, rumble and all four trigger effects work | observed and felt |
| `status[0]` really is at absolute offset 54 | plugging in USB-C flipped its high nibble 0 → 1 while the rival candidate bytes at 43/44 did not move |
| Battery range | read 0 when flat, 10 after charging |
| Accelerometer scale | vector magnitude measured 8186 against the expected 8192 = 1 g |
| Gyro | ±1000 range when moved, near zero at rest |
| Touchpad | a drag tracked (1133,809) → (1827,432) with a stable contact id |
| The L2CAP payload really starts at `p_buf->offset` | the base detector picked 9 every time, and `offset` was also 9 |
| ESP32-S3 has no Bluetooth Classic | esptool reports `Features: WiFi, BLE`; only the original ESP32 has `CONFIG_BT_CLASSIC_ENABLED=y` in the installed SDK |

## Bluetooth transport

`ds_bt_bluedroid` was run against a real DualSense on an original ESP32
(Arduino-ESP32 3.3.11):

```
[link] connecting
[link] CONNECTED
0x01 ... bat=n/a:0      compatibility report, status correctly reported absent
0x31 ... bat=10         switched itself after the first output report
```

The switch from 0x01 to 0x31 is the strongest single check available without a
protocol analyser: the pad only does it after accepting an output report, and
it drops any report whose CRC32 is wrong. The transport never touches the CRC
or the 0xA2 transaction byte — it writes whatever `ds_build_output()` produced
straight to the interrupt channel — so the switch proves the core's framing is
right end to end.

Output was then sent continuously at ~10 Hz with the sequence tag advancing
every frame and the link stayed up, which also exercises the sequence-inside-CRC
handling.

Re-run on 2026-08-14 through the `DualSense32` wrapper rather than the raw
transport (`DualSense32 pad(ds_bt_bluedroid());`, flash fully erased, paired
with Create+PS): connected, and holding Cross drove the light bar and both
motors. The only line that differs from the dongle sketch beside it is the
constructor argument, which is what the class was for.

The same board then ran it with no address at all. Handed NULL, the link
inquires, matches `ds_hci_cod_is_gamepad` — the filter the dongle route
already used, which lives in `ds_hci_host.c` and has no target guard — and
pages what it finds. A pad held in pairing mode was found and connected, and
after a reset the same pad came back on its PS button through the inbound
path, which never needed an address.

That run also fixed a byte-order bug worth recording, because the seam is
ambiguous by nature: `ds_transport_t::begin()` carries a BD_ADDR in **HCI wire
order**, the reverse of the way an address is written. The dongle link speaks
raw HCI and wants exactly that; Bluedroid wants written order and reverses it
itself. A link that gets this wrong pages an address nobody has and reports
nothing, for ever.

`examples/Troubleshooting/EventLog` additionally runs six consistency checks on every
frame and prints only when they disagree. A full pass over the controller —
all buttons, both sticks to their stops, both triggers, a touchpad drag,
motion — produced no `!! INCONSISTENT` lines.

### Gotcha: Arduino releases the Bluetooth heap

`esp_bt_controller_init()` fails with `ESP_ERR_NO_MEM` unless something tells
Arduino to keep the ~36 KB of controller memory it otherwise frees before
`setup()`. The core signals this with a constructor in
`esp32-hal-alloc-bt-classic-mem.h`, which only works from C++. From C, define
the documented strong override instead:

```c
bool btClassicInUse(void) { return true; }
```

## HCI packet layer

`ds_hci.c` is the groundwork for driving a USB Bluetooth dongle on an
ESP32-S3, which has no Bluetooth Classic radio of its own. It builds and parses
raw HCI packets and knows nothing about USB, an RTOS or a state machine.

Source: Bluetooth Core Specification, Vol 4 Part E (HCI Functional
Specification) and Vol 4 Part B (HCI USB Transport Layer).

`examples/SelfTest/HciPackets` runs 133 assertions with no dongle attached. As with
the protocol core, the expected byte strings in `vectors.h` are written out
from the specification field lists by `gen_hci_vectors.py`, independently of
the implementation.

| Board | Result |
|---|---|
| ESP32 (original), Arduino-ESP32 3.3.11 | **133/133 PASS** |
| ESP32-S3 (LOLIN S3 Mini), Arduino-ESP32 3.3.11 | **133/133 PASS** |

Coverage: opcode packing, every command builder against a spec-derived vector,
the parameter and buffer rejection paths, command complete and command status,
connection setup and teardown events, all three inquiry result variants, the
secure simple pairing events, ACL framing, ACL reassembly including the
malformed cases, and address formatting both ways.

### Two traps this layer exists to avoid

**USB carries no packet type byte.** Over UART every HCI packet is prefixed
with 0x01 command / 0x02 ACL / 0x04 event. Over USB the endpoint says what the
packet is and that byte does not exist: commands go out as the data stage of a
class control transfer (`bmRequestType` 0x20, `bRequest` 0x00), events arrive
on interrupt IN, ACL on bulk. Prepending a type byte would be read as the low
half of an opcode.

**The three inquiry result events are not the same shape.** Adding RSSI in
0x22 dropped one of the two reserved bytes in 0x02, so the class of device and
clock offset sit one byte earlier. Only the address and the page scan
repetition mode share an offset. The self-test parses both variants of the same
device and requires both to yield 0x002508.

## HCI host state machine

`ds_hci_host.c` is the conversation on top of those packets: bring a
controller up, find a DualSense, page it, pair with it and hold an encrypted
ACL link. It calls two functions to put bytes on the wire and is handed every
event and ACL packet that comes back. It never blocks, allocates or reads a
clock.

That last part is what makes it testable. `examples/SelfTest/HciHost` replaces
the wire with a recording function and replays the entire conversation turn by
turn: **218 assertions, no dongle and no controller attached.**

| Board | Result |
|---|---|
| ESP32 (original), Arduino-ESP32 3.3.11 | **218/218 PASS** |
| ESP32-S3 (LOLIN S3 Mini), Arduino-ESP32 3.3.11 | **218/218 PASS** |

The events fed in come from the specification field lists and the expected
replies were decided from the specification, not read out of the
implementation — see `gen_host_vectors.py`.

Covered end to end:

| Scenario | What is checked |
|---|---|
| Setup | all nine commands in order, byte for byte, and the version, buffer size and address read back off the controller |
| Setup failure | a rejected step stops the sequence instead of carrying on; an unrelated Command Complete is not mistaken for the current step |
| Pairing | page, authenticate, no stored key, IO capability exchange, Just Works confirmation, link key handed to the app, encryption |
| Reconnect | a stored key short-circuits the pairing dance |
| Failures | refused page, page timeout, failed authentication — each stops without attempting the next step |
| Inbound | a DualSense paging us is accepted and joins the same flow |
| Inquiry | results decoded, gamepad class recognised, completion returns to ready |
| ACL receive | reassembled and handed up on the right channel; a foreign handle is ignored |
| ACL send | a 79 byte report over a 32 byte buffer fragments 28/32/19, and pushing the three fragments back through the reassembler reproduces the original exactly |
| Flow control | credits are spent, refused when exhausted, returned by Number Of Completed Packets, and clamped to the controller's total |

### Setup order is not arbitrary

Reset first, because a dongle that was just plugged in may be mid-anything.
Read Buffer Size before any ACL is sent, or there is no way to know how to
fragment or how many packets may be in flight. Simple pairing mode before scan
enable, so we never advertise a mode we cannot honour.

And the event mask is set to all ones rather than left alone: **the
specification default masks off the secure simple pairing events**, and
without IO Capability Request and User Confirmation Request a modern pairing
stalls with no error anywhere.

## L2CAP

`ds_l2cap.c` opens the two channels a DualSense actually talks over: PSM
0x0011 for HID control and PSM 0x0013 for HID interrupt, which is where the
input and output reports flow. It sits directly on the host layer — `send`
goes to `ds_hci_host_send_l2cap`, and `ds_l2cap_handle` is fed from the
host's `on_l2cap`.

Source: Bluetooth Core Specification, Vol 3 Part A (L2CAP).

`examples/SelfTest/L2cap` replays the whole channel handshake against a
recording function: **144 assertions, no radio.**

| Board | Result |
|---|---|
| ESP32 (original), Arduino-ESP32 3.3.11 | **144/144 PASS** |
| ESP32-S3 (LOLIN S3 Mini), Arduino-ESP32 3.3.11 | **144/144 PASS** |

Signalling identifiers are deliberately not baked into the expected bytes. The
test checks the command code and the body, then separately checks that every
response echoes the identifier of the request it answers — pinning the
identifier into a vector would only test the counter.

| Scenario | What is checked |
|---|---|
| Handshake | connection request, response, both configuration directions, and that a channel opens only when **both** halves have agreed |
| Auto sequencing | interrupt is opened by itself once control is up, and only then |
| Traffic | reports handed up by PSM, sent to the remote's channel id, unknown ids dropped, sends past the peer's MTU refused |
| Inbound | a DualSense opening the channel itself is accepted and configured; an unsupported PSM is refused |
| Config edges | an unknown hint is ignored, an unknown real option is rejected with the offending option echoed back, an invalid channel id draws a Command Reject |
| MTU renegotiation | a peer refusing 672 and offering 185 gets asked again for 185 — once, then the channel closes |
| Teardown | both directions of disconnection, and a link drop closing everything without touching the wire |
| Signalling | echo, information request, unknown command, **two commands packed in one packet**, and truncated packets dropped rather than read past |

### The field that is easy to get backwards

L2CAP names channel endpoints "source" and "destination", and which is which
flips between the request and the response. The rule that makes it consistent:
**the source cid is always the initiator's endpoint.** So a Configuration
Response we send in reply to the peer carries the *peer's* cid, while one we
receive carries *ours* — which is why both lookups in the code key on the
local cid.

## The transport seam

`ds_transport_t` was always the intended shape; all three links now fit it.

| Link | Behind it |
|---|---|
| `ds_bt_bluedroid` | Bluedroid L2CAP on the original ESP32 |
| `ds_link_usb_hci` | `ds_usb_hci` + `ds_hci` + `ds_hci_host` + `ds_l2cap` |
| `ds_link_usb_hid` | `ds_usb_hid` |

`examples/ButtonsAndSticks` uses the seam and nothing else. Flashed to an
ESP32-S3-DevKitC-1 with the flash erased, so no bond and no stored anything:

```
link: connecting
link: connecting
paired - next time just press PS
link: connected
X
X
```

Under a hundred lines drove a scan, a pairing, a bond written to NVS, the two
HID channels and button reports. Everything between the sketch and the USB
pipes is behind one call.

Two things stayed out of the transport on purpose. **Bond storage** is the
application's: whether a key belongs in NVS, in a file or nowhere is not a
protocol question, so the link hands keys out through `on_bond` and takes them
back through `set_bond`. **Feature reports** do not fit an interface whose
whole job is input reports, so calibration and the pad's Bluetooth address are
link-specific accessors rather than a widened struct.

## The live link, measured

Running `examples/Troubleshooting/DongleStepByStep` with a five second pulse of counters, on a link
that had been up for six minutes:

```
PULSE  in 80596 (+1966)  out ok 2031 fail 0  credits 9/9
PULSE  in 84529 (+1967)  out ok 2131 fail 0  credits 8/9
PULSE  in 88466 (+1966)  out ok 2230 fail 0  credits 9/9
```

| | |
|---|---|
| inbound | 1966 reports per five seconds, **about 393 a second**, and steady to within a handful |
| outbound | every send accepted, none refused |
| ACL credits | back at the full nine between sends, dipping to eight while one is in flight |

That rules out the two things worth suspecting when a link dies slowly: the
credit accounting is not leaking, and the USB pipes are not silting up.

### What a link actually says when it ends

`ds_hci_host` keeps the reason byte from Disconnection Complete and the status
from Connection Complete, because they are the only places the answer exists:

| | |
|---|---|
| `remote user ended it` (0x13) | the pad chose to go |
| `connection timeout` (0x08) | the radio link was lost — neither side ended it |
| `ended by the local host` (0x16) | we did |

A link that dropped here reported 0x08, and every reconnection afterwards
failed. That combination has one common cause and it is not a protocol bug.

### Divergent link keys are unrecoverable from one side

A DualSense remembers exactly one host. If its key and the host's stop
matching — a half-finished pairing, a reflash that kept NVS while the pad
re-paired elsewhere — then every reconnection fails authentication, forever,
and the symptom is a link that connects and instantly vanishes.

Neither side can fix it alone. Both have to forget:

| | |
|---|---|
| ESP32 | upload with the flash erased (`EraseFlash=all` in the FQBN), or send `c` to the sketch |
| DualSense | Create + PS until the bar flashes, which replaces its stored host |

After doing both, the same build ran for minutes with the numbers above.

### The pad does not open its own channels

A bonded DualSense pages the host when PS is pressed, and then waits. It does
**not** open the HID L2CAP channels itself, so the host has to — whichever end
started the ACL link. This was worth measuring: holding back to let the pad go
first adds a wait to every single reconnection and gains nothing.

L2CAP still gives way if both ends do request the same PSM at once, because a
peer whose request is refused gives up on the whole link. An attempt of ours
that has not opened yet is worth less than a request already in hand — but an
**open** channel is not surrendered, since traffic is already flowing on it.
Both cases are in `SelfTest/L2cap`.

## USB link, measured

`examples/Troubleshooting/PadFeatureReports` drives a DualSense plugged straight into the S3. This
is what finally verified `DS_LINK_USB`, which until now had only ever been
transcribed from the Linux structs.

```
[1] controller attached, 054C:0CE6
    bluetooth address 24:A6:FA:xx:xx:xx
    calibration: gyro x1105920/17701  accel bias(-6,-174,-18)
[2] ready
    REPORT    0x01  (usb, 64 bytes)
    BATTERY   9  full
    MOTION    gyro(   3248,   -187,     62)  |a|=8216
```

Descriptors, read off the pad:

```
vid:pid 054C:0CE6   Sony Interactive Entertainment
                    DualSense Wireless Controller
4 interfaces, 500 mA
  interface 0        class 01/01/00   audio control
  interface 1 alt1   class 01/02/00   isoc OUT 392   speaker
  interface 2 alt1   class 01/02/00   isoc IN  196   microphone
  interface 3        class 03/00/00   HID
    ep 0x84  interrupt IN   64  interval 4
    ep 0x03  interrupt OUT  64  interval 4
```

The HID interface is found by class rather than by number, because three audio
interfaces sit in front of it and a later revision need not keep that order.

### Feature reports

| Report | Size | Contents | Checked by |
|---|---|---|---|
| 0x09 | 20 | the pad's own Bluetooth address at offset 1 | it read `24:A6:FA:xx:xx:xx`, which is the address the same pad announces over the air |
| 0x05 | 41 | factory motion calibration | applying it puts the accelerometer at `\|a\|` 8216 against 8192 for 1 g |

The address comes out in HCI wire order, so it can go straight to
`ds_hci_addr_to_str` or `ds_hci_cmd_create_connection` without reversing.
That is the useful half of reading it: a pad identified on a wire can be paged
over the air without an inquiry.

### The two sensors are not in the same units

This cost a bug and is worth stating plainly.

| | off the wire | after `ds_apply_calibration` |
|---|---|---|
| accelerometer | 8192 LSB per g | 8192 LSB per g — the correction is about a percent |
| gyroscope | **about 16 LSB per deg/s**, varying by unit | **1024 LSB per deg/s** |

So calibrating the gyro multiplies it by roughly sixty. This measurement gave
`1105920 / 17701` = 62.5.

`ds_motion_t.gyro` was 16 bits, which at 1024 LSB per deg/s runs out at 32
deg/s. Shaking the controller pinned all three axes at once:

```
gyro(-32768,-16675, 32767)
gyro( 32767, 32767, 32767)
```

The field is now `int32_t`, and the multiply is done in 64 bits because a
reading near the end of its range times a numerator near a million overflows
int32 on the way through. The header used to claim raw gyro was already
1024 LSB per deg/s; it never was — that is the calibrated figure, and the
accelerometer being right in both units is what made the mistake easy.

## End to end: a DualSense on an ESP32-S3

`examples/Troubleshooting/DongleStepByStep` wires all five layers together and runs on an
ESP32-S3-DevKitC-1 with the dongle in the OTG port. The S3 has no Bluetooth
Classic radio of its own; everything above the dongle is this library.

```
[   0.83s] [1] dongle attached, 33FA:0012
[   0.93s] [2] controller ready: hci version 14, manufacturer 2279
[   0.94s]     address 04:7F:0E:01:91:D0, acl buffer 9 x 679
[   0.95s]     hci: inquiring
[   9.89s]     seen 24:A6:FA:xx:xx:xx  class 002508   <- gamepad
[   9.89s] [3] connecting
[  10.02s]     hci: acl up, authenticating
[  10.04s]     hci: no stored key, pairing
...
[  31.76s] L119,156 R129,127  L2  0 R2255  ........ bat 9
[  32.38s] L  1,34  R129,127  L2  0 R2  0  ........ bat 9
```

Everything in that trace is this stack talking to a bare radio:

| Step | Proved |
|---|---|
| nine setup commands answered | commands go out on the control pipe and Command Complete comes back on interrupt IN |
| `manufacturer 2279`, `hci version 14` | read by this code, and the same values Linux reports for the same model |
| `acl buffer 9 x 679` | Read Buffer Size parsed, so ACL flow control has real numbers |
| a nearby ESP32 found during inquiry at its known address | the inquiry result parser is decoding real air traffic |
| `class 002508` recognised as a gamepad | class-of-device decoding |
| paging, authentication, secure simple pairing, encryption | the whole `ds_hci_host` state machine against a real peer |
| HID control then interrupt channel open | `ds_l2cap` against a real peer |
| stick and trigger values tracking the hand | `ds_protocol` end to end over a link this library built from HCI up |

No firmware was uploaded to the dongle at any point.

### Gotcha: interrupt IN transfers must be one packet each

The first run crashed inside the host controller driver:

```
assert failed: _buffer_parse_intr hcd_dwc.c:2424
  (rem_len == 0 && desc_status == USB_DWC_HAL_XFER_DESC_STS_SUCCESS)
```

ESP-IDF splits an interrupt transfer into packet-sized descriptors and asserts
if the device ends the transfer early with a short packet. Asking for 272
bytes and receiving a six byte event does exactly that. So `ds_usb_hci`
submits `num_bytes = wMaxPacketSize` and reassembles whole events itself from
the code and length bytes. Bulk endpoints have no such limitation — one
transfer handles a short packet fine, so ACL is read in one piece.

## USB host, measured

`examples/Troubleshooting/WhatIsOnUSB` brings up the ESP-IDF USB Host Library on an
ESP32-S3-DevKitC-1 and describes whatever is plugged into the OTG port. Run
against the dongle this project targets, with the UART port carrying the log:

```
line check   D+ (gpio20) = 1   D- (gpio19) = 0
  -> full speed device attached, data lines connected

  vid:pid        33FA:0012
  device class   E0/01/01
  product        UGREEN BT6.0 Adapter
  speed          full

configuration: 2 interfaces, 100 mA
  interface 0 alt 0  class E0/01/01  3 endpoints   <-- Bluetooth HCI
    ep 0x81  interrupt    IN   max 16  interval 1
    ep 0x02  bulk         OUT  max 64  interval 1
    ep 0x82  bulk         IN   max 64  interval 1
  interface 1 alt 0..5   isochronous   SCO audio, not needed
```

That is the textbook layout the HCI layer was written against, read off the
device itself rather than from a datasheet:

| Pipe | Endpoint | Carries |
|---|---|---|
| control | endpoint 0, 64 byte packets | HCI commands |
| interrupt IN | 0x81, 16 byte packets | HCI events |
| bulk OUT | 0x02, 64 byte packets | ACL out |
| bulk IN | 0x82, 64 byte packets | ACL in |

**No firmware was uploaded.** The device enumerated and reported its product
string with nothing loaded into it, which is what the Barrot chip family was
expected to do and removes the firmware-upload stage from the plan. Proof that
the radio itself works still has to wait for an HCI Reset drawing a Command
Complete.

### Gotcha: a null enumeration filter leaves the device unconfigured

Arduino-ESP32 3.3.11 ships its ESP-IDF libraries with
`CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y`. If
`usb_host_config_t.enum_filter_cb` is then left NULL, enumeration gets as far
as addressing the device and stops. The symptoms are quietly misleading:

- `usb_host_lib_info()` reports one device
- `usb_host_device_addr_list_fill()` lists none
- `usb_host_device_open(client, 1, ...)` nonetheless succeeds
- the device descriptor reads fine, so endpoint 0 is demonstrably working
- `usb_host_get_active_config_descriptor()` hands back **ESP_OK and a null
  pointer**, which crashes anything that trusts the return code alone
- no `USB_HOST_CLIENT_EVENT_NEW_DEV` ever arrives, and nothing is logged,
  because the shipped libraries are built with `CONFIG_LOG_MAXIMUM_LEVEL=1`

The fix is four lines:

```c
static bool enumFilter(const usb_device_desc_t *dd, uint8_t *configValue) {
  *configValue = 1;
  return true;
}
hostCfg.enum_filter_cb = enumFilter;
```

Before finding that, the configuration descriptor was read manually over
endpoint 0 with `usb_host_transfer_submit_control()`, which is worth keeping in
the probe: it proves the control pipe works independently of the library's
enumeration, and it is the same call HCI commands will use.

### Dongle survey

A UGREEN BT6.0 adapter reported by another owner on Arch Linux, which is the
model this work targets:

| Observation | Consequence |
|---|---|
| `ID 33fa:0012 UGREEN BT6.0 Adapter` | the VID/PID to match on |
| two interfaces, `Class=Wireless`, `Driver=btusb` | standard 0xE0/01/01 HCI device, no vendor transport needed |
| `btmgmt info`: `version 14`, br/edr in both supported and current settings | HCI 14 is Bluetooth 6.0, and BR/EDR is live rather than BLE-only |
| `Supported phys: BR1M1SLOT ... EDR2M5SLOT` | the basic rate PHYs a DualSense needs are present |
| `manufacturer 2279` | not Realtek (93), Broadcom (15), Intel (2) or MediaTek, i.e. none of the families btusb uploads firmware for, so the firmware is likely on-chip |

Unconfirmed until a unit is in hand: whether any firmware upload is needed, and
whether a retail unit still reports 33fa:0012.

## Not yet verified

- **USB link.** `DS_LINK_USB` layouts follow the Linux structs but no
  DualSense has been plugged into a USB host running this code yet.
- **Motion calibration.** Feature report 0x05 is never read, so gyro and accel
  are raw and each gyro axis carries a factory bias of a few hundred LSB.
- **Trigger effects beyond the four wrapped here** (Bow, Galloping, Machine).
  The block parameter takes a raw effect, so they are reachable without a
  wrapper.

## DualShock 4 over USB — 2026-08-15

Hardware: ESP32-S3-DevKitC-1, pad in the native USB port. The pad enumerated
as **VID 054C PID 09CC** (CUH-ZCT2), which is the id the link maps to
`DS_DEVICE_DUALSHOCK4`.

`5_SelfTest/Ds4Core` — **130 assertions, all passing.** Vectors laid out from
the structs in Linux `hid-playstation.c`, CRC from zlib.

Every input offset was then confirmed against the raw 64-byte report rather
than assumed. Each of these is a field that could only read this way if the
offset is right:

| Field | Offset (from the report id) | What confirms it |
|---|---|---|
| sticks | 1-4 | `81 80 83 84` — all four within one count of centre |
| buttons | 5, 6, 7 | byte 7 steps by exactly −0x20 between samples 248 reports apart, which is the 6-bit counter in bits 2-7 wrapping — it can only be `buttons[2]` |
| L2 / R2 | 8, 9 | L2 reads 0 untouched |
| sensor timestamp | 10-11, **two bytes** | the 16-bit value moves 56 496 counts per second; at the pad's 5.33 µs tick one second is 187 617, and 187 617 mod 65 536 = 56 481 |
| temperature | 12 | `0x18` = 24 °C |
| gyro | 13-18 | reads −8, −3, −6 at rest, i.e. zero |
| accel | 19-24 | −113, 8005, 1746 → magnitude **8194** against `DS_ACCEL_LSB_PER_G` 8192, so exactly 1 g |
| status | 30 | `0x15` → battery 5, cable bit set |
| touch | 33 onwards | `num_touch_reports` 0 and both contact bytes `0x80`, i.e. nothing touching |

Calibration, feature report **0x02, 37 bytes**, parsed and applied:

```
02 00 00 F9 FF 00 00 3B 22 E7 DD AC 22 3C DD 33 23 AD DC 1C 02
1C 02 05 20 1E E0 9A 20 66 E0 FF 1F E2 DF 08 00
```

gyro speed ± are both 540, so `numer` = 1080 × 1024 = **1 105 920**, and the
three denominators come out 17 492 / 17 776 / 18 054 — a scale of about 63,
which is the factor between raw and calibrated gyro on this pad.

### One thing that is the pad, not the code

R2 rests at **73-79** with its digital bit set, on a pad nobody is touching;
L2 rests at a clean 0. Every neighbouring field decodes correctly, so this is
the controller's own trigger sitting partly depressed rather than an offset
being off by one. Worth knowing before reading anything into it: the
`DualShock4` example drives the rumble from R2, so on this pad it buzzes
continuously, and a running rumble motor is enough to swing the gyro ±16 °/s
and wobble two accelerometer axes.

## DualShock 4 on the original ESP32's own radio — 2026-08-16

Confirmed by the owner on hardware. All three links now carry a DualShock 4:
wired USB, a USB Bluetooth dongle, and Bluetooth Classic on the original chip.

It did not work at first, and neither cause was in the DualShock 4 support.

**ds_bt_request_full_report() was never called.** Defined, exported, and
called by nothing — not the library, not an example. So on this link a pad
stayed on input report 0x01 indefinitely: sticks, buttons and triggers, and
none of the motion, touchpad or battery that only the full report carries.
0x01 is also the report *both* controllers send, so nothing could tell them
apart and a DualShock 4 was announced as a DualSense.

This was never a DualShock 4 problem. **A DualSense on this route had the same
missing motion, touchpad and battery**, and had done for as long as the link
has existed — it simply looked like a working controller, because the fields
it was missing are the ones nobody notices are absent.

**The device defaulted to DS_DEVICE_DUALSENSE**, conflating "this is a
DualSense" with "nothing has said yet". Over Bluetooth the pad is only named
by its first full report, so that window always exists; reading the default
inside it is what produced the wrong answer rather than no answer.
ds_transport_t::device_named separates them now.

### Still not read on this link

The factory motion calibration. The link asks for feature report 0x05 — that
is what switches the pad to full reports — and discards the answer. Gyro is
therefore raw, about sixty times smaller than calibrated. The accelerometer is
within a percent either way, so a resting pad still totals 1 g. The dongle
route reads it properly and is the comparison.

**Fixed later the same day.** ctrl_data_ind() was three lines that freed the
buffer unread, so the reply to the library's own request was thrown away. It
now parses the report and, because the calibration arrives before the pad has
named itself and the two controllers order the gyro fields differently, keeps
the raw bytes and reads them again on identification. Confirmed on hardware:
accelerometer magnitude 1.001 g and a resting gyro within half a degree per
second, which only both hold if DualShock 4 offsets were used *and* the
calibration was re-read after identification.


## begin() expectations, held output, identity on disconnect — 2026-08-23

ESP32-S3 with a USB Bluetooth dongle, DualShock 4, log on COM6. Everything
below is from that session; nothing here is inferred.

### Self-test

`5_SelfTest/Ds4Core`, run on the board: **185 passed, 0 failed**, including the
eleven new assertions covering the expectation — that it seeds `device` without
setting `device_named`, that a disconnect returns to it, that a real
identification replaces it, and that expecting a DualShock 4 while a DualSense
names itself is what `mismatched()` reports.

### The naming window is real, and it is tens of milliseconds

Two connections, everything sampled before a single character was printed:

| | first | second |
|---|---|---|
| `deviceNamed()` at connect | no | no |
| `device()` reads as | DualSense (the default) | DualSense |
| `send()` at that moment | **held** | **held** |
| all three sampled within | 0 ms | 0 ms |
| named after | **32 ms** | **23 ms** |
| and it is a | DualShock 4 | DualShock 4 |

So for 23-32 ms after `connected()` goes true, nothing has said which pad it
is, and `device()` returns a default rather than an answer. That window is
exactly what made a DualShock 4 announce itself as a DualSense before
`device_named` existed.

`send()` returning held is the new gate. Before it, this call built a DualSense
output report, sent it to a DualShock 4 which ignored it, and cleared the
pending flag — losing the setting rather than delivering it late.

The light bar was set to red **once**, inside that window, and never again. It
came on red after the pad named itself, which is the delivery half of the same
claim.

### Disconnect drops the identity

`deviceNamed()` reads no immediately after the pad powers off, and each
reconnection prints a fresh `named after`. The name is worked out again from
the first full report rather than carried over.

### A measurement that lied, recorded because the trap is easy to fall into

The first run of the check reported `SENT (not held!)` on one connection out of
three — with `deviceNamed()` printed as no on the same line.

That combination cannot happen: all five places that write `device` set
`device_named` in the same breath. The fault was in the check. `deviceNamed()`
was a snapshot from the top of the loop, while `send()` was reached two
`Serial.printf` calls later — and at 115200 baud a line of text costs several
milliseconds against a window of tens. On the run where naming took 12 ms
rather than 23, printing overtook it.

Sampling all three before printing changed the result to held on every
connection, with `all three sampled within 0 ms` to show the race was gone.
Nothing in the library changed between the two runs.

### The three new examples, on a DualShock 4 over the cable

**EitherPad.** Named as a DualShock 4 from the product id immediately;
capabilities listed a light bar it blinks itself and nothing else; `micLed()`
refused and said so. `battery 0/10` at first, which was the example's fault
and not the library's — see below.

**Ds4Touchpad.** Two contacts tracked at once with separate ids. Coordinates
reach 0 and 1918 across and 941 down, so `TOUCH_MAX_Y = 942` is right for this
pad. Gestures after the rewrite, all as intended:

| done | reported | dx, dy, ms |
|---|---|---|
| tap x3 | `tap` x3 | (+9,+48) (+15,+3) (+9,-21), 116-132 ms |
| swipe right x2 | `swipe right` x2 | +1752, +1911 |
| swipe left x2 | `swipe left` x2 | -1850, -1918 |
| swipe down x2 | `swipe down` x2 | +918, +924 |
| swipe up x2 | `swipe up` x2 | -857, -885 |
| one slow drag | `swipe down` | (+287,+290) over 9089 ms |

The first version of that sketch never reported a swipe at all. It called the
classifier when a movement passed 300 counts and the classifier called anything
under 383 a tap, so everything in between was a tap and the start point was
reset each time, which stopped a slow drag ever accumulating. Two thresholds,
the outer one looser than the inner. It compiled perfectly throughout.

**Ds4Lightbar.** The four states cycle and the pad keeps its rhythm through a
three-second `delay()` in `loop()`, which is the claim the sketch exists to
make. The idle state was wrong — see below.

### Announcing before a report has arrived

`EitherPad`, `2_USBDirect/DualShock4`, `Ds4Lightbar` and `Ds4Touchpad` printed a
battery of 0 on connect. Probed directly:

    first connected frame: status.valid=0 battery=0 charging=0
    t=1s   status.valid=1  battery=7  charging=0
    t=2s   status.valid=1  battery=8  charging=1

Nothing has been parsed on the first connected frame, so the snapshot is still
zeroed. **`deviceNamed()` is not a proxy for "a report has arrived."** Over USB
the product id names the pad the moment it attaches, ahead of any input report;
over Bluetooth the pad is named *by* a report, so the two coincide there. That
is why only the USB examples were wrong, and why `1_Dongle/DualShock4` — which
already waited for a report id — was right. All four now wait for
`raw().report_id != 0`.

### lightbarBlink(0, 0) is off, not solid

The header and both manuals said zero and zero returned the light bar to solid.
It does not. Pressing one button per row, on a DualShock 4 over the cable:

| sent | seen |
|---|---|
| nothing at all | dark |
| red + `blink(6,6)` | red, blinking |
| blue + `blink(0,0)`, one report | **dark** |
| `blink(0,0)` sent, then blue sent separately | **dark** |
| blue only, blink left at (6,6) | blue, blinking |

So a colour change does apply while blinking — the last row proves it — but
zero and zero extinguishes the bar and no colour sent with or after it comes
back. Corrected in `DualSense32.h` and in both manuals.

### The cause: an off of zero

The owner found it, by noticing that the result depended on *when* the button
was pressed: sending while the bar was dark left it dark, and a second press
finished the job. Three probes then pinned it down.

| sent, one report per press | seen |
|---|---|
| `blink(255,0)` then colour, separately | first press stops the blink, colour needs a second press |
| the same state four times, 50 ms apart | no better -- all four land inside one 2.55 s cycle |
| fast blink to latch the colour, then `blink(255,0)` | the last report is ignored; stays on the fast blink |
| colour + `blink(200,1)` | **colour and rate both change, first press** |
| colour + `blink(100,2)` | same, but the 20 ms of dark is visible |
| colour + `blink(255,0)` | still needs two presses |

**A DualShock 4 takes a new colour or a new rate only at a blink boundary.** An
off of zero leaves it with no boundary to arrive at, so the bar holds what it
was showing and ignores everything sent afterwards until something else
disturbs it -- which in practice meant the next button press, seconds later.
Both zero is worse: it puts the bar out.

So there is no "solid" call, and none is needed. A long on with an off of 1
reads as lit and behaves:

    pad.lightbarBlink(200, 1);   // 2 s lit, 10 ms dark

Ten milliseconds of dark cannot be seen. Twenty can -- `blink(100,2)` was
reported as a visible flicker, which is what fixes the boundary between the two.

Corrected in `DualSense32.h` and in both manuals, and
`2_USBDirect/Ds4Lightbar` now rests at `(200, 1)` rather than the `(200, 15)`
it was given while this was still unexplained. All four of its states were then
confirmed on the pad.

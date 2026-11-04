# AGENTS.md

Read `docs/MANUAL-en.md` first. It is the reference for this library — the API,
the three links, the board settings and the troubleshooting all live there, and
this file deliberately repeats none of it. What is here is only the part an
agent needs and a manual written for a person holding hardware cannot carry.

## Building

The manual describes the Arduino IDE's menus. These are the equivalents.

    # 3_BTClassic/* and 4_Troubleshooting/EventLog — the original ESP32
    arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
        examples/3_BTClassic/Basics

    # everything else — ESP32-S3
    arduino-cli compile --fqbn esp32:esp32:esp32s3 examples/2_USBDirect/Basics

`PartitionScheme=huge_app` is not optional on the original ESP32. On the default
scheme the sketch overflows and the build fails with `text section exceeds
available space`.

Picking the wrong board is the likeliest reason a build fails here. An ESP32-S3
has no Bluetooth Classic radio, so on an S3 `ds_bt_bluedroid()` is not declared
at all and the error names that rather than the real mistake.

All 26 examples build. Fewer than 26 means something is broken.

`arduino-cli` may not be on PATH; the Arduino IDE bundles a copy, on Windows
under `resources/app/lib/backend/resources/`. It resolves this library from the
sketchbook's `libraries/` directory — compiling against a clone kept somewhere
else will not find it.

## What you cannot verify

Anything that needs the controller itself: pairing, rumble, the light bar, real
input values, and every reconnection behaviour. A build that succeeds says the
code compiles and nothing more. Do not report it as working.

The sketches under `examples/5_SelfTest/` are the exception worth knowing. They
check the protocol core against recorded reports, so they need a board to run on
but no controller. `ProtocolCore` and `Ds4Core` cover the most ground.

## Repository conventions

- `CHANGES.md` is in `.gitignore` on purpose. It stays on disk and must never be
  committed or reach the history.
- Record every change in `CHANGES.md`, including what was checked on hardware
  and what was not.
- Commit messages in English. No `Co-Authored-By` trailers.
- Never `git push --tags`. A local-only backup tag points at pre-rewrite history
  that still contains `CHANGES.md`. Push tags by name.

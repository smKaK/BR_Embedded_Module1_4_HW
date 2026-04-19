

https://github.com/user-attachments/assets/5e92ba68-e496-438b-bb5d-fe24497b339d



# PoliceFlash

Two-channel police-style LED flasher for the **ESP32-S3-DevKitC-1**, built with PlatformIO + Arduino framework. Runs several blink patterns and lets you switch between them live — over the serial console or via hardware buttons — with a pluggable input + debounce stack behind the scenes.

## Hardware

- **MCU:** ESP32-S3 @ 240 MHz (variant `N16R8` — 16 MB Flash, 8 MB PSRAM)
- **Board:** `esp32-s3-devkitc-1`
- **LEDs:**
  - Red → **GPIO 4**
  - Blue → **GPIO 5**
- **Buttons (active-low, `INPUT_PULLUP`):**
  - External button → **GPIO 2** (interrupt-gated sampling)
  - On-board BOOT button → **GPIO 0** (polled sampling)

Wire each LED through a current-limiting resistor to ground. Wire each external button between its GPIO and GND.

## Runtime Control

After flashing, open the serial monitor at **115200 baud**. Either type a command and press Enter, or press one of the buttons:

| Input            | Effect                               |
|------------------|--------------------------------------|
| `p0`             | Alternating — red/blue ping-pong     |
| `p1`             | DoubleBlinkPolice *(default)*        |
| `p2`             | SOS — Morse `... --- ...` on both    |
| `?`              | Print current pattern name           |
| any other text   | Print help                           |
| External / BOOT  | Cycle to the next pattern            |

## Architecture

The firmware splits into three cooperating layers. `Flasher` still drives the LEDs from pattern frames; a new `ModeManager` owns the pattern registry and the list of input controllers; each controller turns its own source (serial / button) into a uniform `ControlEvent` stream.

```
main.cpp
  │
  ├── Flasher  ──  ticks through the active IPattern and writes masks to the output
  │     ├── IOutputStrategy  ←  MultiPinOutput (digitalWrite on N pins)
  │     └── IPattern         ←  AlternatingPattern
  │                             DoubleBlinkPolicePattern
  │                             SosPattern
  │
  └── ModeManager  ──  polls IControllers every tick, dispatches ControlEvents to Flasher
        ├── SerialController           ←  parses "p0/p1/p2/?" from UART
        └── ButtonController (x N)     ←  pin → Debouncer → edge → Command::NextPattern
              ├── IDebounceAlgo        ←  Hysteresis / Integrator / ShiftRegister
              └── IButtonSampler       ←  PollingSampler / InterruptSampler
```

All wiring is done with designated-initializer `Config` structs, e.g.:

```cpp
pflash::ButtonController gExtBtn({
    .pin       = kPinExtButton,
    .debouncer = &gExtDeb,
    .sampler   = &gExtSampler,
    .label     = "ext",
});

pflash::ModeManager gModes({
    .flasher      = &gFlasher,
    .patterns     = { &gAlternating, &gDoubleBlink, &gSos },
    .controllers  = { &gSerialCtl, &gExtBtn, &gBootBtn },
    .defaultIndex = pflash::idx(pflash::PatternId::DoubleBlinkPolice),
});
```

### Debouncing

When a mechanical button is pressed or released, the contact doesn't cleanly snap from one level to the other — it rattles for a few milliseconds, producing a burst of fake edges. A debounce algorithm watches the noisy raw signal and answers one question: *has the line really changed, or is this still bounce?* Each of the three algorithms shipped here gives a different answer to that question, and each one is a legitimate choice depending on what you care about (latency, noise rejection, or CPU cost).

#### HysteresisDebounce — "wait until it settles"

The idea: trust a new level only once it has held steady for long enough. As long as the line keeps flipping, the timer keeps resetting and the output stays put; the moment the raw signal stops changing and stays at the new value for `stableMs`, we commit.

This is the most intuitive model — it maps directly onto how a human would explain debouncing ("wait a bit, then look"). Latency is predictable: every real press costs a fixed `stableMs` delay. Noise has to be persistent to get through, but a single very brief spike is still ignored because it resets the timer and then disappears.

Best when you want simple, bounded, wall-clock-based behavior and the exact sample cadence isn't under your control.

#### IntegratorDebounce — "weight of evidence"

The idea: treat each raw sample as one vote. High samples push a counter up, low samples pull it down, and the counter is clamped at both ends. The committed level only flips when the counter reaches the top (enough "high" votes in a row to overpower any recent "low" votes) or the bottom. Between the extremes, the output keeps its last value — so isolated noise samples just nudge the counter a little and quickly get cancelled out.

This one doesn't care about time — it cares about how lopsided the recent sample history is. A single spurious sample only costs ±1, so brief spikes barely move the needle. But it also means the commit delay depends on how often you sample, not on wall-clock time: sample twice as fast and it commits twice as quickly.

Best when samples come at a roughly fixed rate and you want graceful degradation against noise rather than a hard "wait N ms" window.

#### ShiftRegisterDebounce — "unanimous consensus"

The idea: keep the last `width` samples in a rolling window and only commit a new level when *all* of them agree. One mismatching sample anywhere in the window is enough to veto the transition. Samples are taken at most once per `sampleIntervalMs`, so the effective commit delay is `width × sampleIntervalMs`.

This is the strictest of the three. It will not commit during any bounce burst, no matter how asymmetric — it needs a completely clean window to flip. The cost is latency: you're essentially waiting for `width` consecutive clean samples. It also scales naturally to very noisy signals by just widening the window or slowing the sample rate.

Best when the signal is very bouncy or electrically noisy and you'd rather pay extra latency than ever commit a wrong level.

#### Picking one

- **Hysteresis** — default. Clean buttons, predictable delay, easy to reason about.
- **Integrator** — sampled at a fixed rate and want the output to "coast" through isolated glitches.
- **ShiftRegister** — noisy environment, want the firmest guarantee that every commit is preceded by a quiet window.

### Sampling strategies

`IButtonSampler::shouldSample(now_ms)` decides each tick whether `ButtonController` should read the pin:

- **PollingSampler** — always samples (simple, slightly more CPU).
- **InterruptSampler** — only samples when a GPIO ISR flagged a change, or while the debouncer is still settling.

### Adding a new pattern

1. Create `include/patterns/MyPattern.h` deriving from `pflash::IPattern`.
2. Create `src/patterns/MyPattern.cpp` with a `constexpr Frame kFrames[]` table — each frame is `{ duration_ms, channel_mask }`.
3. Add it to `PatternId` (keeps serial `pN` indices in sync) and include it in the `ModeManager` `patterns` list in `main.cpp`.

A `Frame`'s `channel_mask` bit `N` maps to the LED at index `N` in the `MultiPinOutput` pin list.

### Adding a new input source

1. Implement `pflash::IController` (`begin()`, `poll()`, `name()`) and emit `ControlEvent`s.
2. Instantiate it and add it to the `ModeManager` `controllers` list.

## Project Layout

```
include/          project headers
├── Flasher.h, FlasherBuilder.h
├── ModeManager.h, PatternId.h
├── IOutputStrategy.h, IPattern.h
├── outputs/      concrete output strategies
├── patterns/     concrete pattern headers
├── inputs/       IController, SerialController, ButtonController, samplers
└── debounce/     Debouncer + IDebounceAlgo implementations
src/              matching .cpp files + main.cpp
lib/              local libraries
test/             unit tests (PlatformIO)
platformio.ini    build environment
```

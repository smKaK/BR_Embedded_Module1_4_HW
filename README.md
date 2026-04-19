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

`Debouncer` is a thin driver that tracks a single `stable` level and reports rising / falling edges to `ButtonController`. It delegates the actual decision of "is this new sample believable?" to an `IDebounceAlgo`. The algorithm interface is tiny — `reset(level)`, `update(rawLevel, now_ms) -> bool`, and a `pending()` hint that tells the driver to keep calling `update` even when the raw level hasn't changed (needed by time-based algorithms). Three implementations ship:

#### HysteresisDebounce — time-based

Constructor: `HysteresisDebounce(stableMs = 20)`.

Holds the current `stable` level and a `candidate` level with a timestamp. Each sample:

- If `raw != candidate`: the candidate changed — adopt the new candidate, restart the timer, keep reporting the old `stable` level.
- If `raw == candidate` and `candidate != stable` and at least `stableMs` have elapsed since the candidate first appeared: promote `candidate` to `stable`.

`pending()` returns true while `candidate != stable`, so the driver keeps ticking it even without fresh samples — that lets the level commit purely from the passage of time. Bounce noise resets the timer and so can never promote.

Trade-off: adds a fixed `stableMs` latency to every real transition, which is the simplest and most predictable behavior. Default `stableMs = 20`, used by both buttons in `main.cpp`.

#### IntegratorDebounce — saturating counter

Constructor: `IntegratorDebounce(maxCount = 5)` (clamped to ≥1).

Maintains a counter in `[0, maxCount]`. Each sample: `+1` if `raw` is high, `-1` if low, clamped at the endpoints. The `stable` output only flips when the counter **saturates** — to `true` when it hits `maxCount`, to `false` when it drops to `0`. Between the endpoints the output keeps its last value, which is what gives this algorithm its strong hysteresis.

`pending()` is always false: no time-only behavior — every update needs a fresh raw sample.

Trade-off: forgiving of isolated spikes (one flipped sample costs only `±1` of accumulated counter) but the commit time depends on *how often* you sample, not on wall-clock time. Good when the caller controls sampling cadence; pair with a periodic sampler for predictable timing.

#### ShiftRegisterDebounce — N-of-N consensus

Constructor: `ShiftRegisterDebounce(width = 8, sampleIntervalMs = 1)` (width clamped to `[1, 32]`).

Takes at most one sample per `sampleIntervalMs`; older ticks inside the same window are ignored. Each taken sample shifts left into a `width`-bit register:

```
reg = ((reg << 1) | raw) & mask        // mask = (1 << width) - 1
```

Output flips to `true` **only** when the register reads all-ones (`width` consecutive high samples at the configured interval), and to `false` only when it reads all-zeros. Any mixed pattern keeps the previous `stable` level.

`pending()` is false; between sample windows the driver can short-circuit. `reset(level)` pre-fills the register with `mask` or `0` so the first real edge still requires `width` fresh samples to confirm.

Trade-off: effectively a tunable FIR filter — commit time is `width × sampleIntervalMs`, and a single opposite sample anywhere inside the window invalidates the consensus. Strongest rejection of fast bounce bursts; slowest to commit.

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

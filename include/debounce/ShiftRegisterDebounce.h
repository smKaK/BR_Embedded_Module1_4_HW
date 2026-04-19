#pragma once

#include "IDebounceAlgo.h"

namespace pflash {

// Samples rawLevel at most every `sampleIntervalMs`, shifts into a `width`-bit
// register (1..32). Output flips to high when the register is all-ones, and
// back to low when it is all-zero — otherwise it keeps its last state.
class ShiftRegisterDebounce final : public IDebounceAlgo {
public:
    explicit ShiftRegisterDebounce(uint8_t width = 8, uint32_t sampleIntervalMs = 1);

    void        reset(bool initialLevel)                   override;
    bool        update(bool rawLevel, uint32_t now_ms)     override;
    // Stays true while the window is not unanimous — the driver must keep feeding
    // samples (the internal interval throttle decides when to actually consume one)
    // even after ISR activity has stopped, or we'd never reach saturation.
    bool        pending() const                            override { return reg_ != 0u && reg_ != mask_; }
    const char* name() const                               override;

private:
    uint8_t  width_;
    uint32_t sampleIntervalMs_;
    uint32_t mask_;          // (1 << width_) - 1
    uint32_t reg_{0};
    uint32_t lastSample_{0};
    bool     stable_{false};
    bool     primed_{false}; // first sample always taken regardless of interval
};

} // namespace pflash

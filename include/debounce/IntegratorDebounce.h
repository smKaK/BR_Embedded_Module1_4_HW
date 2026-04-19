#pragma once

#include "IDebounceAlgo.h"

namespace pflash {

// Saturating up/down counter: +1 per high sample, -1 per low, clamped to [0, max].
// Output flips to high when counter saturates at max, back to low when it drops to 0.
// Self-throttles to at most one vote per sampleIntervalMs so the commit time is
// bounded (maxCount * sampleIntervalMs) regardless of how often update() is called.
class IntegratorDebounce final : public IDebounceAlgo {
public:
    explicit IntegratorDebounce(uint8_t maxCount = 5, uint32_t sampleIntervalMs = 1);

    void        reset(bool initialLevel)                   override;
    bool        update(bool rawLevel, uint32_t now_ms)     override;
    // Stays true while the counter is mid-range — the driver must keep feeding
    // us samples, even without ISR activity, until we saturate one way or the other.
    bool        pending() const                            override { return count_ != 0 && count_ != maxCount_; }
    const char* name() const                               override;

private:
    uint8_t  maxCount_;
    uint32_t sampleIntervalMs_;
    uint8_t  count_{0};
    uint32_t lastSample_{0};
    bool     primed_{false};
    bool     stable_{false};
};

} // namespace pflash

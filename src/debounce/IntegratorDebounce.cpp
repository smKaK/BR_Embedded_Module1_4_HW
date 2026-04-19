#include "debounce/IntegratorDebounce.h"

namespace pflash {

IntegratorDebounce::IntegratorDebounce(uint8_t maxCount, uint32_t sampleIntervalMs)
    : maxCount_(maxCount == 0 ? 1 : maxCount),
      sampleIntervalMs_(sampleIntervalMs) {}

void IntegratorDebounce::reset(bool initialLevel) {
    stable_     = initialLevel;
    count_      = initialLevel ? maxCount_ : 0;
    lastSample_ = 0;
    primed_     = false;
}

bool IntegratorDebounce::update(bool rawLevel, uint32_t now_ms) {
    if (primed_ && (now_ms - lastSample_) < sampleIntervalMs_) {
        return stable_;
    }
    lastSample_ = now_ms;
    primed_     = true;

    if (rawLevel) {
        if (count_ < maxCount_) { ++count_; }
    } else {
        if (count_ > 0)         { --count_; }
    }
    if (count_ == maxCount_)    { stable_ = true;  }
    else if (count_ == 0)       { stable_ = false; }
    return stable_;
}

const char* IntegratorDebounce::name() const {
    return "Integrator";
}

} // namespace pflash

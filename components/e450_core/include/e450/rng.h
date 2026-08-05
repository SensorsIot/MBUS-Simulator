#pragma once

#include <cstdint>

namespace e450 {

// A deterministic pseudo-random generator — FR-MOD-04.
//
// "Random" here means *unpredictable downstream*, not unpredictable to us.
// Mode 3 varies its values so that a decoder cannot pass by caching a constant
// or guessing; it must decode every message. But FR-CLK-04 still requires two
// runs from reset to produce identical bytes, so the sequence has to be
// reproducible. A seeded generator is both: it cannot be anticipated from the
// bus, and we can replay it exactly.
//
// xorshift32. Not cryptographic, and it does not need to be — nothing here is
// a secret, and the property being bought is repeatability.
class Rng {
  public:
    explicit constexpr Rng(uint32_t seed) : state_(seed == 0 ? DEFAULT_SEED : seed) {}

    uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    // Inclusive of both bounds. Modulo bias is real and irrelevant at these
    // ranges — the values only have to vary, not be uniform.
    uint32_t in_range(uint32_t lo, uint32_t hi) {
        if (hi <= lo) return lo;
        return lo + next() % (hi - lo + 1);
    }

  private:
    static constexpr uint32_t DEFAULT_SEED = 0x2545F491u;
    uint32_t state_;
};

// The seed mode 3 starts from at every boot. Fixed, so the run is replayable.
inline constexpr uint32_t MODE3_SEED = 0xE450C11u;

}  // namespace e450

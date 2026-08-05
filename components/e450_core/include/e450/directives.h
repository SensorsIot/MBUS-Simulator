#pragma once

#include <array>
#include <cstdint>

namespace e450 {

// Runtime directives — FSD §14.
//
// Everything here is settable from the console and nothing is a build option.
// That is the point rather than a convenience: a fault that needs a rebuild
// makes the firmware image part of the test fixture, and a bench run then
// cannot say which simulator produced its evidence. `[user]`

// Which identity object, if any, the telegram carries (FR-FLT-10).
enum class Identity : uint8_t { None, Ldn, DevId };

// Persistent settings. These are not corruptions — they are legitimate meter
// behaviours a decoder has to cope with — so unlike a fault they stay set until
// changed.
struct EmissionConfig {
    // 0 keeps the wall-clock-aligned period of FR-SCH-01. Non-zero switches to
    // free-running with exactly this much silence between telegrams, which is
    // the only way to place a boundary at 1999 vs 2000 ms (FR-FLT-08).
    uint32_t gap_ms = 0;

    uint8_t serial_chars = 8;  // 8 or 16 (FR-FLT-11)
    Identity identity = Identity::None;

    // Overrides the model's cumulative import energy when set (FR-FLT-12).
    // A separate flag rather than a sentinel, because 0 Wh is a real value.
    bool energy_set = false;
    uint32_t energy_wh = 0;

    // Non-zero switches registers to the self-describing form that carries a
    // scaler (FR-FLT-13).
    int8_t scaler = 0;
};

// One-shot faults. Exactly one can be armed, and it clears itself after the
// telegram it applies to — a sticky corruption silently poisons every later
// case in a run. `[user]`
enum class Fault : uint8_t {
    None = 0,
    Fcs,                // arg = 1-based frame number
    Truncate,           // arg = 1-based frame number
    DropBlock,          // arg = 1-based block number
    StartMidFrame,      // no argument
    DuplicateRegister,  // obis = the register to repeat
    Noise,              // arg = byte count
};

struct FaultSpec {
    Fault kind = Fault::None;
    uint32_t arg = 0;
    std::array<uint8_t, 6> obis{};

    [[nodiscard]] bool armed() const { return kind != Fault::None; }
};

// "none", or "fcs:2", "noise:64", "start-mid-frame" — the form that goes in the
// emission report, so a test can assert on what was actually applied.
// `out` must have room for 32 characters.
const char *fault_to_string(const FaultSpec &fault, char *out, std::size_t len);

}  // namespace e450

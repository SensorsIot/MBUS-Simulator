#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "e450/rng.h"
#include "e450/telegram.h"
#include "e450/value_model.h"

namespace e450 {

// The three things this simulator can put on the wire — FSD §13.
//
// They are a diagnostic ladder, not a feature list. Each mode adds exactly one
// layer to the one below it, so the first one that fails names the fault:
//
//   Pattern  bytes, no protocol         does any signal come off the line?
//   Simple   one frame, real values     is a single telegram decodable?
//   E450     the full list, varying     can it decode *this meter's* telegrams?
//   Ramp     counted bytes, no protocol *how many* bytes were lost, exactly?
//
// Between Simple and E450 lie the two things most likely to break a decoder:
// multi-block General Block Transfer reassembly, and values it has not seen
// before. Running them in order separates "the line is wrong" from "the decoder
// is wrong" from "the decoder cannot handle the real thing".
enum class Mode : uint8_t {
    Pattern = 1,
    Simple = 2,
    E450 = 3,
    Ramp = 4,
};

// Mode 1 — FR-MOD-02. A burst of 0x55 on the same cadence as a telegram, not a
// continuous stream: a transmission is delimited by the silence after it, so a
// stream with no end is one nothing downstream ever reports on. A delimited
// burst is the entire point of the mode.
inline constexpr uint8_t PATTERN_BYTE = 0x55;
inline constexpr std::size_t PATTERN_BURST_BYTES = 218;  // one second at 2400 8E1

// Mode 2 — FR-MOD-03. The smallest thing that is still a real telegram: a push
// setup and two registers, fixed values, one GBT block, one HDLC frame. Fixed
// because the point is a known answer — these two numbers can be checked
// against without reading a log from the simulator at all.
namespace simple {

inline constexpr std::size_t OBJECT_COUNT = 3;
inline constexpr uint32_t ACTIVE_POWER_W = 777;
inline constexpr uint32_t ACTIVE_ENERGY_WH = 12345;

std::span<const CaptureObject> objects();
std::vector<TelegramValue> values();

}  // namespace simple

// Mode 4 — FR-MOD-06. `00 01 02 … FF` repeating, for exactly as many bytes as a
// mode 3 telegram occupies, then the same silence.
//
// The whole value is in the first byte received. It *is* the number of bytes
// lost off the front of the burst: receive `0x19` and 25 bytes went missing, no
// arithmetic and no second log to correlate against. Every other way of
// measuring this subtracts a count taken on one device from a count taken on
// another and hopes both refer to the same burst.
//
// Loss is therefore measured modulo 256 — beyond that the count wraps and the
// answer is ambiguous. At 2400 baud, 256 bytes is 1.2 s of line time, so a
// wrap means something far larger than a lead-in problem is wrong.
std::vector<uint8_t> ramp_bytes(std::size_t count);

// Mode 3 — FR-MOD-04. Choose the next load, then let the model integrate it.
//
// Called *before* the tick that integrates the period, so the energy reported
// in a telegram is exactly the power reported in that same telegram multiplied
// by the period. The arithmetic checks against itself, with no reference to
// what came before.
void apply_random_load(ValueModel &model, Rng &rng);

}  // namespace e450

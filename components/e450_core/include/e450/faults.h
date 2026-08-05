#pragma once

#include <cstdint>
#include <vector>

#include "e450/directives.h"

namespace e450 {

// Wire-level faults — FSD §14, FR-FLT-02..05, FR-FLT-07.
//
// These operate on the framed telegram rather than on its content, because that
// is what they are: a frame whose FCS is wrong is still a correctly built frame
// that was damaged. Keeping them here, after encoding, means the encoder has no
// idea faults exist and cannot be made wrong by them.
//
// `frames` is one entry per HDLC frame; the result is the byte stream to put on
// the line. With Fault::None it is a plain concatenation, which is exactly what
// encode_telegram does — so a fault-free emission goes down the same path as a
// faulted one, and the path itself is not a variable.
// `preamble` bytes of filler are laid down first, inside the same burst
// (FR-FLT-16), and Fault::StartMidFrame cuts from the front of the whole burst
// — preamble included. That is not an oversight: a lead-in that is long enough
// to absorb the truncation is exactly what a preamble is *for*, so the
// interaction is the mitigation being tested, not a fault being defeated.
std::vector<uint8_t> apply_wire_fault(const std::vector<std::vector<uint8_t>> &frames,
                                      const FaultSpec &fault, uint32_t preamble = 0,
                                      uint8_t preamble_byte = 0x7E);

// The default filler. HDLC treats repeated flags between frames as inter-frame
// fill, so 0x7E is what a real lead-in would look like — but see the
// characterisation in the host tests before assuming it is *safe*: a decoder is
// entitled to reject `7E 7E` as a zero-length frame, and one does.
inline constexpr uint8_t PREAMBLE_BYTE = 0x7E;

// FR-FLT-07. `count` bytes containing no 0x7E and no valid framing, so a
// receiver sees a burst arrive and finds nothing in it. Deterministic, so a
// failing run can be replayed.
std::vector<uint8_t> noise_bytes(uint32_t count);

// How many bytes of frame 1 FR-FLT-05 skips. Enough to land inside the
// information field rather than in the header, which is the case worth testing:
// a receiver that resynchronises on the next 0x7E has to discard a partial
// frame first.
inline constexpr std::size_t START_MID_FRAME_SKIP = 40;

}  // namespace e450

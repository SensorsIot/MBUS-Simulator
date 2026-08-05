#include "e450/faults.h"

#include <cstdio>

namespace e450 {
namespace {

// Bytes of a frame that are not information: opening flag, format, length, two
// address bytes, source, control, HCS — then FCS and closing flag at the end.
constexpr std::size_t FRAME_HEADER = 9;
constexpr std::size_t FRAME_TRAILER = 3;  // FCS lo, FCS hi, closing flag

void append(std::vector<uint8_t> &out, const std::vector<uint8_t> &frame) {
    out.insert(out.end(), frame.begin(), frame.end());
}

}  // namespace

std::vector<uint8_t> noise_bytes(uint32_t count) {
    std::vector<uint8_t> out;
    out.reserve(count);
    // A fixed repeating pattern, deliberately avoiding 0x7E so nothing
    // downstream can mistake any of it for a frame boundary, and deliberately
    // not 0x55 so it cannot be confused with mode 1's pattern.
    static constexpr uint8_t SOUP[] = {0x3C, 0xA5, 0x11, 0xF0, 0x69, 0xC3, 0x2D, 0x84};
    for (uint32_t i = 0; i < count; ++i) out.push_back(SOUP[i % sizeof(SOUP)]);
    return out;
}

std::vector<uint8_t> apply_wire_fault(const std::vector<std::vector<uint8_t>> &frames,
                                      const FaultSpec &fault) {
    std::vector<uint8_t> out;

    if (fault.kind == Fault::Noise) return noise_bytes(fault.arg);

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const std::size_t number = i + 1;  // directives are 1-based
        std::vector<uint8_t> frame = frames[i];

        switch (fault.kind) {
            case Fault::DropBlock:
                // FR-FLT-04. One GBT block per frame, so dropping block k is
                // dropping frame k — the APDU then never completes and no
                // measurement set can be recovered from it.
                if (number == fault.arg) continue;
                break;

            case Fault::Fcs:
                // FR-FLT-02. Invert both FCS bytes: still two bytes in the right
                // place, so the frame is well formed and only its checksum is
                // wrong. Every other frame stays intact, which is the property
                // the test needs.
                if (number == fault.arg && frame.size() >= FRAME_TRAILER) {
                    frame[frame.size() - 3] = static_cast<uint8_t>(~frame[frame.size() - 3]);
                    frame[frame.size() - 2] = static_cast<uint8_t>(~frame[frame.size() - 2]);
                }
                break;

            case Fault::Truncate:
                // FR-FLT-03. Cut inside the information field: no FCS, no
                // closing flag, nothing that could be mistaken for a short but
                // complete frame.
                if (number == fault.arg) {
                    const std::size_t keep = FRAME_HEADER + (frame.size() - FRAME_HEADER -
                                                             FRAME_TRAILER) / 2;
                    frame.resize(keep < frame.size() ? keep : frame.size());
                }
                break;

            case Fault::StartMidFrame:
                // FR-FLT-05. Begin partway through frame 1, as if reception
                // started mid-transmission. Later frames are untouched, so a
                // receiver that resynchronises recovers everything after the
                // first flag it finds.
                if (number == 1) {
                    const std::size_t skip =
                        START_MID_FRAME_SKIP < frame.size() ? START_MID_FRAME_SKIP : 0;
                    frame.erase(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(skip));
                }
                break;

            case Fault::None:
            case Fault::DuplicateRegister:  // content-level, applied before encoding
            case Fault::Noise:              // handled above
                break;
        }

        append(out, frame);
    }

    return out;
}

const char *fault_to_string(const FaultSpec &fault, char *out, std::size_t len) {
    switch (fault.kind) {
        case Fault::None: std::snprintf(out, len, "none"); break;
        case Fault::Fcs: std::snprintf(out, len, "fcs:%u", unsigned(fault.arg)); break;
        case Fault::Truncate: std::snprintf(out, len, "truncate:%u", unsigned(fault.arg)); break;
        case Fault::DropBlock: std::snprintf(out, len, "drop-block:%u", unsigned(fault.arg)); break;
        case Fault::StartMidFrame: std::snprintf(out, len, "start-mid-frame"); break;
        case Fault::Noise: std::snprintf(out, len, "noise:%u", unsigned(fault.arg)); break;
        case Fault::DuplicateRegister:
            std::snprintf(out, len, "duplicate-register:%u.%u.%u.%u.%u.%u", fault.obis[0],
                          fault.obis[1], fault.obis[2], fault.obis[3], fault.obis[4],
                          fault.obis[5]);
            break;
    }
    return out;
}

}  // namespace e450

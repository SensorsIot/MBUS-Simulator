#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "e450/datetime.h"

namespace e450 {

// One entry of the push object list — COSEM capture_object_definition, encoded
// on the wire as structure[4] { class_id uint16, OBIS octet-string[6],
// attribute int8, data_index uint16 }. FR-ENC-02, shape measured from both
// captures.
struct CaptureObject {
    uint16_t class_id;
    std::array<uint8_t, 6> obis;
    int8_t attribute;
    uint16_t data_index;
};

// The value transmitted for one capture object, positionally (FR-ENC-03).
//
// Three kinds is all this meter needs: the push-setup object whose value *is*
// the object list, the logical name (an octet string), and every register (a
// uint32 in W, Wh, mA or varh — FR-ENC-04, no scaler on the wire).
struct TelegramValue {
    enum class Kind : uint8_t { ObjectListArray, OctetString, Uint32 };

    Kind kind = Kind::Uint32;
    uint32_t u32 = 0;
    std::vector<uint8_t> octets;

    static TelegramValue object_list() { return {Kind::ObjectListArray, 0, {}}; }
    static TelegramValue octet(std::span<const uint8_t> bytes) {
        return {Kind::OctetString, 0, {bytes.begin(), bytes.end()}};
    }
    static TelegramValue uint32(uint32_t v) { return {Kind::Uint32, v, {}}; }
};

struct TelegramSpec {
    std::span<const CaptureObject> objects;
    std::span<const TelegramValue> values;  // exactly one per object
    uint32_t invoke_id = 0;
    DateTime timestamp{};
};

// Wire-layer constants. Every default was measured from e450_serial8.hex and
// e450_serial16.hex, which agree on all of them (FSD Appendix B). They are
// parameters rather than literals so a test can vary the block size — the split
// policy is ours to choose (FSD A-03), and a different split is itself a useful
// case for the DUT's reassembler.
// Meter-link line timing (FR-LNK-01). One character is start + 8 data + even
// parity + stop, so 2400 baud carries 218.18 byte/s and nothing faster.
inline constexpr int LINK_BAUD = 2400;
inline constexpr int LINK_BITS_PER_CHAR = 11;

// How long `bytes` occupy the line, rounded up: a partial character still holds
// it, and rounding down would let a budget check pass on a telegram that does
// not fit.
constexpr uint32_t wire_time_ms(std::size_t bytes) {
    const std::size_t bits = bytes * LINK_BITS_PER_CHAR;
    return static_cast<uint32_t>((bits * 1000 + LINK_BAUD - 1) / LINK_BAUD);
}

// Silence left in a push period once a telegram of `bytes` has been sent —
// negative if it does not fit. The DUT delimits transmissions by a gap rather
// than by frame count (FR-SCH-08), so this is the quantity that decides whether
// consecutive telegrams are read as two or misread as one.
constexpr int32_t silence_ms(std::size_t bytes, uint32_t period_ms) {
    return static_cast<int32_t>(period_ms) - static_cast<int32_t>(wire_time_ms(bytes));
}

// The gap a reader needs to call a transmission finished, and the margin this
// simulator holds itself to. 2000 ms is the DUT's own threshold; the simulator
// targets 3000 ms so that a reader running slightly long, or a telegram that
// grows a little, does not silently merge two cycles into one.
inline constexpr int32_t MIN_SILENCE_MS = 3000;
inline constexpr int32_t DUT_BURST_TIMEOUT_MS = 2000;

struct WireConfig {
    std::array<uint8_t, 2> destination{0xCE, 0xFF};
    uint8_t source = 0x03;
    uint8_t control = 0x13;
    // Bytes of APDU per General Block Transfer block (FR-ENC-07).
    std::size_t block_payload = 128;
};

// Stage 1 — object list + values -> DataNotification APDU (FR-ENC-01..05).
std::vector<uint8_t> encode_apdu(const TelegramSpec &spec);

// Stage 2 — the security stage of FR-ENC-13. In v1 the CII is not encrypted
// (FSD A-01), so this is the identity function. It exists as a named step rather
// than as an absence: if EBL ever enables AES-GCM-128, this is the one place
// that changes, and everything either side of it stays put.
std::vector<uint8_t> apply_security(std::vector<uint8_t> apdu);

// Stage 3 — split into GBT blocks (FR-ENC-06/07). Streaming mode, block control
// 0x40, final block 0xC0, numbered from 1.
std::vector<std::vector<uint8_t>> segment_gbt(std::span<const uint8_t> apdu,
                                              std::size_t block_payload);

// Stage 4 — one HDLC type-3 frame around one GBT block (FR-ENC-08..10). The LLC
// header precedes the GBT header in the first frame of a telegram only
// (FR-ENC-09).
std::vector<uint8_t> frame_hdlc(std::span<const uint8_t> information,
                                const WireConfig &wire);

// The whole pipeline: one telegram, ready for the UART.
std::vector<uint8_t> encode_telegram(const TelegramSpec &spec, const WireConfig &wire = {});

}  // namespace e450

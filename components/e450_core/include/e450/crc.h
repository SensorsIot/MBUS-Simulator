#pragma once

#include <cstdint>
#include <span>

namespace e450 {

// CRC-16/CCITT as HDLC uses it: polynomial 0x8408 (reflected 0x1021), init
// 0xFFFF, final XOR 0xFFFF — commonly catalogued as CRC-16/X-25. FSD Appendix B,
// measured from both real captures.
//
// Used for both the HCS and the FCS; they differ only in what they cover.
uint16_t crc16_x25(std::span<const uint8_t> data);

}  // namespace e450

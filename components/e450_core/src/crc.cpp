#include "e450/crc.h"

namespace e450 {

// Bitwise rather than table-driven: a 512-byte table would buy speed this
// project has no use for — the line is 2400 baud, so a whole telegram takes
// 1.6 s to transmit and microseconds to checksum.
uint16_t crc16_x25(std::span<const uint8_t> data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408) : static_cast<uint16_t>(crc >> 1);
        }
    }
    return static_cast<uint16_t>(crc ^ 0xFFFF);
}

}  // namespace e450

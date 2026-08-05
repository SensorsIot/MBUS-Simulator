#include "e450/telegram.h"

#include <algorithm>

#include "e450/crc.h"

namespace e450 {
namespace {

constexpr uint8_t TAG_ARRAY = 0x01;
constexpr uint8_t TAG_STRUCTURE = 0x02;
constexpr uint8_t TAG_UINT32 = 0x06;
constexpr uint8_t TAG_OCTET_STRING = 0x09;
constexpr uint8_t TAG_INT8 = 0x0F;
constexpr uint8_t TAG_UINT16 = 0x12;

constexpr uint8_t APDU_DATA_NOTIFICATION = 0x0F;
constexpr uint8_t APDU_GENERAL_BLOCK_TRANSFER = 0xE0;

constexpr uint8_t GBT_STREAMING = 0x40;
constexpr uint8_t GBT_STREAMING_LAST = 0xC0;

constexpr uint8_t HDLC_FLAG = 0x7E;
constexpr uint8_t HDLC_FORMAT_TYPE3 = 0xA0;

// Everything after the format byte that is not information: length byte, two
// address bytes, source, control, HCS and FCS. The length field counts from the
// format byte through the FCS, so it is information + 10 (FSD Appendix B).
constexpr std::size_t HDLC_OVERHEAD = 10;

constexpr uint8_t LLC[3] = {0xE6, 0xE7, 0x00};

void put_u16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

// BER definite length. Under 128 it is one byte; above, a count-of-bytes byte
// with the high bit set. Needed because a 128-byte GBT block crosses exactly
// that boundary — the default block size is the first value that does.
void put_ber_length(std::vector<uint8_t> &out, std::size_t len) {
    if (len < 0x80) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFF) {
        out.push_back(0x81);
        out.push_back(static_cast<uint8_t>(len));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
    }
}

void put_octet_string(std::vector<uint8_t> &out, std::span<const uint8_t> bytes) {
    out.push_back(TAG_OCTET_STRING);
    put_ber_length(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_object_list(std::vector<uint8_t> &out, std::span<const CaptureObject> objects) {
    out.push_back(TAG_ARRAY);
    put_ber_length(out, objects.size());
    for (const CaptureObject &obj : objects) {
        out.push_back(TAG_STRUCTURE);
        put_ber_length(out, 4);
        out.push_back(TAG_UINT16);
        put_u16(out, obj.class_id);
        put_octet_string(out, obj.obis);
        out.push_back(TAG_INT8);
        out.push_back(static_cast<uint8_t>(obj.attribute));
        out.push_back(TAG_UINT16);
        put_u16(out, obj.data_index);
    }
}

}  // namespace

std::vector<uint8_t> encode_apdu(const TelegramSpec &spec) {
    std::vector<uint8_t> apdu;
    apdu.reserve(512);

    apdu.push_back(APDU_DATA_NOTIFICATION);

    // long-invoke-id-and-priority. The top byte carries priority and service
    // class bits; both captures hold it at 0x00, so the counter is the low 24
    // bits (FR-ENC-05).
    put_u32(apdu, spec.invoke_id & 0x00FFFFFFu);

    // date-time: a length byte and 12 bytes, with no type tag — this field is
    // not a Data, it is an OCTET STRING in the DataNotification structure.
    apdu.push_back(12);
    std::array<uint8_t, 12> when{};
    encode_datetime(spec.timestamp, when);
    apdu.insert(apdu.end(), when.begin(), when.end());

    apdu.push_back(TAG_STRUCTURE);
    put_ber_length(apdu, spec.values.size());
    for (const TelegramValue &value : spec.values) {
        switch (value.kind) {
            case TelegramValue::Kind::ObjectListArray:
                put_object_list(apdu, spec.objects);
                break;
            case TelegramValue::Kind::OctetString:
                put_octet_string(apdu, value.octets);
                break;
            case TelegramValue::Kind::Uint32:
                apdu.push_back(TAG_UINT32);
                put_u32(apdu, value.u32);
                break;
            case TelegramValue::Kind::Raw:
                apdu.insert(apdu.end(), value.octets.begin(), value.octets.end());
                break;
        }
    }

    return apdu;
}

std::vector<uint8_t> apply_security(std::vector<uint8_t> apdu) {
    // FR-ENC-13. Identity: this installation pushes plaintext (FSD A-01).
    return apdu;
}

std::vector<std::vector<uint8_t>> segment_gbt(std::span<const uint8_t> apdu,
                                              std::size_t block_payload) {
    if (block_payload == 0) block_payload = 1;

    std::vector<std::vector<uint8_t>> blocks;
    const std::size_t count = (apdu.size() + block_payload - 1) / block_payload;

    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t offset = index * block_payload;
        const std::size_t len = std::min(block_payload, apdu.size() - offset);
        const bool last = (index + 1 == count);

        std::vector<uint8_t> block;
        block.reserve(len + 8);
        block.push_back(APDU_GENERAL_BLOCK_TRANSFER);
        block.push_back(last ? GBT_STREAMING_LAST : GBT_STREAMING);
        put_u16(block, static_cast<uint16_t>(index + 1));  // block number, from 1
        put_u16(block, 0);                                 // acknowledged block
        put_ber_length(block, len);
        block.insert(block.end(), apdu.begin() + static_cast<std::ptrdiff_t>(offset),
                     apdu.begin() + static_cast<std::ptrdiff_t>(offset + len));
        blocks.push_back(std::move(block));
    }

    return blocks;
}

std::vector<uint8_t> frame_hdlc(std::span<const uint8_t> information, const WireConfig &wire) {
    const std::size_t length = information.size() + HDLC_OVERHEAD;

    std::vector<uint8_t> frame;
    frame.reserve(length + 2);

    frame.push_back(HDLC_FLAG);
    frame.push_back(static_cast<uint8_t>(HDLC_FORMAT_TYPE3 | ((length >> 8) & 0x07)));
    frame.push_back(static_cast<uint8_t>(length & 0xFF));
    frame.push_back(wire.destination[0]);
    frame.push_back(wire.destination[1]);
    frame.push_back(wire.source);
    frame.push_back(wire.control);

    // HCS covers the format byte through the control byte; both checksums are
    // transmitted little-endian (FR-ENC-10).
    const uint16_t hcs = crc16_x25(std::span<const uint8_t>(frame).subspan(1));
    frame.push_back(static_cast<uint8_t>(hcs & 0xFF));
    frame.push_back(static_cast<uint8_t>(hcs >> 8));

    frame.insert(frame.end(), information.begin(), information.end());

    const uint16_t fcs = crc16_x25(std::span<const uint8_t>(frame).subspan(1));
    frame.push_back(static_cast<uint8_t>(fcs & 0xFF));
    frame.push_back(static_cast<uint8_t>(fcs >> 8));

    frame.push_back(HDLC_FLAG);
    return frame;
}

std::vector<std::vector<uint8_t>> encode_telegram_frames(const TelegramSpec &spec,
                                                         const WireConfig &wire) {
    const std::vector<uint8_t> apdu = apply_security(encode_apdu(spec));
    const std::vector<std::vector<uint8_t>> blocks = segment_gbt(apdu, wire.block_payload);

    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(blocks.size());
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        std::vector<uint8_t> information;
        // The LLC header appears once per telegram, not once per frame — the
        // continuation frames carry the GBT header directly (FR-ENC-09).
        if (index == 0) information.insert(information.end(), std::begin(LLC), std::end(LLC));
        information.insert(information.end(), blocks[index].begin(), blocks[index].end());
        frames.push_back(frame_hdlc(information, wire));
    }
    return frames;
}

std::vector<uint8_t> encode_telegram(const TelegramSpec &spec, const WireConfig &wire) {
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t> &frame : encode_telegram_frames(spec, wire)) {
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

}  // namespace e450

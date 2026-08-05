#include "e450/modes.h"

namespace e450 {
namespace simple {
namespace {

// Same class ids and attributes as the EBL list — this is a smaller telegram,
// not a different dialect. 40 is the push setup, 3 is Register.
constexpr uint16_t CLASS_PUSH_SETUP = 40;
constexpr uint16_t CLASS_REGISTER = 3;
constexpr int8_t ATTR_VALUE = 2;

constexpr CaptureObject OBJECTS[OBJECT_COUNT] = {
    // The push setup, whose value is the object list itself. Without it the
    // values that follow are unlabelled numbers that nothing can place.
    {CLASS_PUSH_SETUP, {0, 8, 25, 9, 0, 0xFF}, ATTR_VALUE, 0},
    // Wirkleistungsbezug +P, watts.
    {CLASS_REGISTER, {1, 0, 1, 7, 0, 0xFF}, ATTR_VALUE, 0},
    // Wirkenergiebezug +A, watt-hours.
    {CLASS_REGISTER, {1, 1, 1, 8, 0, 0xFF}, ATTR_VALUE, 0},
};

}  // namespace

std::span<const CaptureObject> objects() { return {OBJECTS, OBJECT_COUNT}; }

std::vector<TelegramValue> values() {
    return {
        TelegramValue::object_list(),
        TelegramValue::uint32(ACTIVE_POWER_W),
        TelegramValue::uint32(ACTIVE_ENERGY_WH),
    };
}

}  // namespace simple

std::vector<uint8_t> ramp_bytes(std::size_t count) {
    std::vector<uint8_t> out;
    out.reserve(count);
    // Starts at zero every burst, without exception: the first byte is only a
    // loss count because the sender's first byte is known to be 0x00.
    for (std::size_t i = 0; i < count; ++i) out.push_back(static_cast<uint8_t>(i & 0xFF));
    return out;
}

void apply_random_load(ValueModel &model, Rng &rng) {
    // A household, not a uniform sample. One draw in eight is export, standing
    // in for a roof that is generating; the rest is consumption. Both are
    // capped where a domestic supply would be.
    const bool exporting = rng.in_range(0, 7) == 0;
    const uint32_t watts = rng.in_range(0, exporting ? 6000 : 9000);

    model.set_import_w(exporting ? 0 : watts);
    model.set_export_w(exporting ? watts : 0);

    // 0.85 to 1.00 inclusive, lagging — an ordinary inductive load. Anything
    // below 1.00 puts the quadrant accumulators to work, so a decoder that
    // ignores reactive energy is caught here rather than in the field.
    const uint32_t hundredths = rng.in_range(85, 100);
    model.set_power_factor(static_cast<float>(hundredths) / 100.0F, true);
}

}  // namespace e450

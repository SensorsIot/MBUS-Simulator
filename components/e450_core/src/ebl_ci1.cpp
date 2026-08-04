#include "e450/ebl_ci1.h"

namespace e450::ebl {
namespace {

// COSEM interface class ids, taken from the captures rather than from a
// standard: 40 is the push-setup object, 3 is Register. Both real telegrams
// agree.
constexpr uint16_t CLASS_PUSH_SETUP = 40;
constexpr uint16_t CLASS_REGISTER = 3;

// Attribute 2 is the value of a Register and the object_list of a push setup;
// attribute 1 is any COSEM object's logical name.
constexpr int8_t ATTR_VALUE = 2;
constexpr int8_t ATTR_LOGICAL_NAME = 1;

constexpr CaptureObject OBJECTS[CI1_OBJECT_COUNT] = {
    // Push setup Consumer Information 1 — its value is the object list itself.
    {CLASS_PUSH_SETUP, {0, 8, 25, 9, 0, 0xFF}, ATTR_VALUE, 0},
    {CLASS_PUSH_SETUP, {0, 8, 25, 9, 0, 0xFF}, ATTR_LOGICAL_NAME, 0},
    // Wirkleistungsbezug +P / Wirkleistungslieferung -P, in watts.
    {CLASS_REGISTER, {1, 0, 1, 7, 0, 0xFF}, ATTR_VALUE, 0},
    {CLASS_REGISTER, {1, 0, 2, 7, 0, 0xFF}, ATTR_VALUE, 0},
    // Wirkenergie +A / -A, in watt-hours.
    {CLASS_REGISTER, {1, 1, 1, 8, 0, 0xFF}, ATTR_VALUE, 0},
    {CLASS_REGISTER, {1, 1, 2, 8, 0, 0xFF}, ATTR_VALUE, 0},
    // Blindenergie by quadrant: +Ri, +Rc, -Ri, -Rc, in var-hours.
    {CLASS_REGISTER, {1, 1, 5, 8, 0, 0xFF}, ATTR_VALUE, 0},
    {CLASS_REGISTER, {1, 1, 6, 8, 0, 0xFF}, ATTR_VALUE, 0},
    {CLASS_REGISTER, {1, 1, 7, 8, 0, 0xFF}, ATTR_VALUE, 0},
    {CLASS_REGISTER, {1, 1, 8, 8, 0, 0xFF}, ATTR_VALUE, 0},
    // Strom L1 / L2 / L3, in milliamperes.
    {CLASS_REGISTER, {1, 0, 31, 7, 0, 0xFF}, ATTR_VALUE, 0},
    {CLASS_REGISTER, {1, 0, 51, 7, 0, 0xFF}, ATTR_VALUE, 0},
    {CLASS_REGISTER, {1, 0, 71, 7, 0, 0xFF}, ATTR_VALUE, 0},
};

// The value of attribute 1 of the push setup is its own logical name — the same
// six bytes the object list declares for it.
constexpr uint8_t PUSH_SETUP_LOGICAL_NAME[6] = {0, 8, 25, 9, 0, 0xFF};

}  // namespace

std::span<const CaptureObject> ci1_objects() { return {OBJECTS, CI1_OBJECT_COUNT}; }

std::vector<TelegramValue> ci1_values(const ValueModel &model) {
    std::vector<TelegramValue> values;
    values.reserve(CI1_OBJECT_COUNT);

    values.push_back(TelegramValue::object_list());
    values.push_back(TelegramValue::octet(PUSH_SETUP_LOGICAL_NAME));

    values.push_back(TelegramValue::uint32(model.import_w()));
    values.push_back(TelegramValue::uint32(model.export_w()));
    values.push_back(TelegramValue::uint32(model.import_wh()));
    values.push_back(TelegramValue::uint32(model.export_wh()));

    values.push_back(TelegramValue::uint32(model.reactive_varh(ValueModel::Q1_IMPORT_LAGGING)));
    values.push_back(TelegramValue::uint32(model.reactive_varh(ValueModel::Q2_EXPORT_LAGGING)));
    values.push_back(TelegramValue::uint32(model.reactive_varh(ValueModel::Q3_EXPORT_LEADING)));
    values.push_back(TelegramValue::uint32(model.reactive_varh(ValueModel::Q4_IMPORT_LEADING)));

    const uint32_t current = model.current_ma();
    values.push_back(TelegramValue::uint32(current));
    values.push_back(TelegramValue::uint32(current));
    values.push_back(TelegramValue::uint32(current));

    return values;
}

}  // namespace e450::ebl

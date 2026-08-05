#include "e450/emission.h"

#include "e450/ebl_ci1.h"
#include "e450/modes.h"

namespace e450 {
namespace {

constexpr uint16_t CLASS_PUSH_SETUP = 40;
constexpr uint16_t CLASS_REGISTER = 3;
constexpr uint16_t CLASS_DATA = 1;
constexpr int8_t ATTR_VALUE = 2;

constexpr std::array<uint8_t, 6> OBIS_PUSH_SETUP{0, 8, 25, 9, 0, 0xFF};
constexpr std::array<uint8_t, 6> OBIS_DEVICE_ID{0, 0, 96, 1, 0, 0xFF};
constexpr std::array<uint8_t, 6> OBIS_LDN{0, 0, 42, 0, 0, 0xFF};
constexpr std::array<uint8_t, 6> OBIS_ACTIVE_POWER{1, 0, 1, 7, 0, 0xFF};
constexpr std::array<uint8_t, 6> OBIS_ACTIVE_ENERGY{1, 1, 1, 8, 0, 0xFF};

// The serial the identity objects carry. Sixteen characters, truncated to eight
// when `serial 8` is set — same digits either way, so a test comparing the two
// forms is comparing lengths and not contents (FR-FLT-11).
constexpr char SERIAL_16[] = "4433781144337811";

void put_identity(Emission &out, const EmissionConfig &cfg) {
    if (cfg.identity == Identity::None) return;

    const std::size_t chars = (cfg.serial_chars == 16) ? 16 : 8;
    std::vector<uint8_t> text(SERIAL_16, SERIAL_16 + chars);

    if (cfg.identity == Identity::DevId) {
        out.objects.push_back({CLASS_DATA, OBIS_DEVICE_ID, ATTR_VALUE, 0});
    } else {
        out.objects.push_back({CLASS_DATA, OBIS_LDN, ATTR_VALUE, 0});
    }
    out.values.push_back(TelegramValue::octet(text));
}

// FR-FLT-06. Repeat one register, with a different value the second time, so
// "the first occurrence was kept" and "the last occurrence was kept" are
// distinguishable outcomes rather than the same number twice.
void put_duplicate(Emission &out, const FaultSpec &fault, uint32_t first_value) {
    if (fault.kind != Fault::DuplicateRegister) return;
    out.objects.push_back({CLASS_REGISTER, fault.obis, ATTR_VALUE, 0});
    out.values.push_back(TelegramValue::uint32(first_value + 111111));
}

uint32_t import_energy(const ValueModel &model, const EmissionConfig &cfg) {
    return cfg.energy_set ? cfg.energy_wh : model.import_wh();
}

}  // namespace

TelegramValue self_describing_register(const std::array<uint8_t, 6> &obis, uint32_t value,
                                       int8_t scaler, uint8_t unit) {
    std::vector<uint8_t> b = {0x02, 0x03, 0x09, 0x06};
    b.insert(b.end(), obis.begin(), obis.end());
    b.push_back(0x06);
    b.push_back(static_cast<uint8_t>(value >> 24));
    b.push_back(static_cast<uint8_t>(value >> 16));
    b.push_back(static_cast<uint8_t>(value >> 8));
    b.push_back(static_cast<uint8_t>(value));
    b.push_back(0x02);
    b.push_back(0x02);
    b.push_back(0x0F);
    b.push_back(static_cast<uint8_t>(scaler));
    b.push_back(0x16);
    b.push_back(unit);
    return TelegramValue::raw(std::move(b));
}

ValueModel fixed_model() {
    ValueModel m;
    m.set_import_w(simple::ACTIVE_POWER_W);
    m.set_power_factor(1.0F, true);
    // 12345 Wh exactly: 12345 Wh = 44 442 000 000 mW·s, and tick() takes
    // milliseconds, so one hour at 12345 W lands on it with no remainder.
    m.tick(3'600'000);
    return m;
}

Emission plan_simple(const EmissionConfig &cfg, const FaultSpec &fault) {
    Emission out;

    if (cfg.scaler != 0) {
        // FR-FLT-13. Self-describing registers carry their own OBIS, so the
        // capture-object list has no part to play and is left empty.
        out.values.push_back(self_describing_register(OBIS_ACTIVE_POWER, simple::ACTIVE_POWER_W, 0,
                                                      UNIT_W));
        out.values.push_back(self_describing_register(
            OBIS_ACTIVE_ENERGY, cfg.energy_set ? cfg.energy_wh : simple::ACTIVE_ENERGY_WH,
            cfg.scaler, UNIT_WH));
        return out;
    }

    out.objects.push_back({CLASS_PUSH_SETUP, OBIS_PUSH_SETUP, ATTR_VALUE, 0});
    out.values.push_back(TelegramValue::object_list());
    put_identity(out, cfg);

    out.objects.push_back({CLASS_REGISTER, OBIS_ACTIVE_POWER, ATTR_VALUE, 0});
    out.values.push_back(TelegramValue::uint32(simple::ACTIVE_POWER_W));

    const uint32_t energy = cfg.energy_set ? cfg.energy_wh : simple::ACTIVE_ENERGY_WH;
    out.objects.push_back({CLASS_REGISTER, OBIS_ACTIVE_ENERGY, ATTR_VALUE, 0});
    out.values.push_back(TelegramValue::uint32(energy));

    put_duplicate(out, fault, energy);
    return out;
}

Emission plan_e450(const ValueModel &model, const EmissionConfig &cfg, const FaultSpec &fault) {
    Emission out;

    if (cfg.scaler != 0) {
        out.values.push_back(
            self_describing_register(OBIS_ACTIVE_POWER, model.import_w(), 0, UNIT_W));
        out.values.push_back(self_describing_register(OBIS_ACTIVE_ENERGY,
                                                      import_energy(model, cfg), cfg.scaler,
                                                      UNIT_WH));
        return out;
    }

    // Appendix A, in transmission order, with the energy override applied.
    const std::span<const CaptureObject> base = ebl::ci1_objects();
    out.objects.assign(base.begin(), base.end());
    out.values = ebl::ci1_values(model);
    if (cfg.energy_set) {
        // Position 5 of the list is 1-1:1.8.0 — see FSD Appendix A. Asserted in
        // the host test rather than trusted, because a reordering of the list
        // would silently move the override onto another register.
        out.values[4] = TelegramValue::uint32(cfg.energy_wh);
    }

    put_identity(out, cfg);
    put_duplicate(out, fault, import_energy(model, cfg));
    return out;
}

}  // namespace e450

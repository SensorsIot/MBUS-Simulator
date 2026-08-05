#pragma once

#include <cstdint>
#include <vector>

#include "e450/directives.h"
#include "e450/telegram.h"
#include "e450/value_model.h"

namespace e450 {

// What one telegram is made of, owned rather than viewed — FSD §14.
//
// The lists are vectors and not spans because the directives make them dynamic:
// `identity` adds an object, `duplicate-register` adds another, `scaler`
// replaces the whole shape. A span into a static table cannot express any of
// that, and a span into a temporary is the bug bind_values() exists to prevent.
struct Emission {
    std::vector<CaptureObject> objects;
    std::vector<TelegramValue> values;

    // Fill a spec that borrows from *this*. The Emission must outlive the spec,
    // which is why this is a member and not a free function taking temporaries.
    [[nodiscard]] TelegramSpec spec(uint32_t invoke_id, const DateTime &when) const {
        TelegramSpec s;
        s.bind_objects(objects);
        s.bind_values(values);
        s.invoke_id = invoke_id;
        s.timestamp = when;
        return s;
    }
};

// The load a fault telegram carries — FR-FLT-01.
//
// Fixed, because an arithmetic assertion cannot be made against a load that
// changes every emission. Faults compose with these values rather than with
// mode 3's generator, so "frame 2 was discarded and the rest decoded to *this*"
// is a statement a test can actually write down.
ValueModel fixed_model();

// The full Appendix A list, with the directives applied (FR-FLT-10..13).
Emission plan_e450(const ValueModel &model, const EmissionConfig &cfg, const FaultSpec &fault);

// The small fixed telegram of mode 2 — one push setup and two registers, one
// GBT block, one HDLC frame.
Emission plan_simple(const EmissionConfig &cfg, const FaultSpec &fault);

// A register in the self-describing form `structure[3] { OBIS, value,
// structure[2]{ scaler, unit } }` — FR-FLT-13.
//
// This is how a scaler becomes observable at all. The capture-object form the
// E450 actually uses carries bare values and no scaler anywhere (FR-ENC-04), so
// a decoder given one has nothing to scale by. This shape is valid DLMS and is
// what plenty of meters emit; it is simply not what *this* meter emits, which is
// why it appears only when `scaler` is set.
TelegramValue self_describing_register(const std::array<uint8_t, 6> &obis, uint32_t value,
                                       int8_t scaler, uint8_t unit);

// Unit enums used by the registers above (DLMS unit table).
inline constexpr uint8_t UNIT_W = 27;
inline constexpr uint8_t UNIT_WH = 30;

}  // namespace e450

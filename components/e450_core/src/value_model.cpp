#include "e450/value_model.h"

#include <cmath>

namespace e450 {
namespace {

// Nominal phase-to-neutral voltage. Not measured and not transmitted — EBL's
// push list carries no voltage registers — so it exists only to derive current
// from power (FR-MDL-07).
constexpr uint32_t NOMINAL_PHASE_VOLTS = 230;
constexpr uint32_t PHASES = 3;

constexpr uint64_t MILLI_UNIT_SECONDS_PER_UNIT_HOUR = 3'600'000ULL;

}  // namespace

void ValueModel::set_power_factor(float pf, bool lagging) {
    if (pf < 0.0F) pf = 0.0F;
    if (pf > 1.0F) pf = 1.0F;
    power_factor_ = pf;
    lagging_ = lagging;
}

void ValueModel::tick(uint32_t elapsed_ms) {
    const auto ms = static_cast<uint64_t>(elapsed_ms);

    // W × ms == mW × s. Exact, no rounding (FR-MDL-04).
    active_[0] += static_cast<uint64_t>(import_w_) * ms;
    active_[1] += static_cast<uint64_t>(export_w_) * ms;

    // Q = P · tan(arccos(pf)). At pf = 1 this is exactly zero, so all four
    // quadrant counters hold (FR-MDL-09).
    if (power_factor_ < 1.0F) {
        const float tan_phi = std::tan(std::acos(power_factor_));
        const auto reactive_var = [tan_phi](uint32_t watts) -> uint64_t {
            return static_cast<uint64_t>(std::llround(static_cast<double>(watts) * tan_phi));
        };

        if (import_w_ > 0) {
            reactive_[lagging_ ? Q1_IMPORT_LAGGING : Q4_IMPORT_LEADING] +=
                reactive_var(import_w_) * ms;
        }
        if (export_w_ > 0) {
            reactive_[lagging_ ? Q2_EXPORT_LAGGING : Q3_EXPORT_LEADING] +=
                reactive_var(export_w_) * ms;
        }
    }
}

uint32_t ValueModel::to_whole_units(uint64_t milli_unit_seconds) {
    return static_cast<uint32_t>(milli_unit_seconds / MILLI_UNIT_SECONDS_PER_UNIT_HOUR);
}

uint32_t ValueModel::current_ma() const {
    const uint64_t total_w = static_cast<uint64_t>(import_w_) + export_w_;
    return static_cast<uint32_t>(total_w * 1000ULL / (PHASES * NOMINAL_PHASE_VOLTS));
}

}  // namespace e450

#pragma once

#include <cstdint>

namespace e450 {

// The quantities a meter would measure, kept mutually consistent — FSD §5.
//
// The correctness claim is that energy is the integral of power. That is what
// makes the simulator falsifiable: "the counter went up" proves nothing, but
// "the counter went up by exactly P × Δt" can fail.
class ValueModel {
  public:
    // Which reactive quadrant accumulates, by the signs of P and Q (FR-MDL-08).
    enum Quadrant : uint8_t {
        Q1_IMPORT_LAGGING = 0,  // +Ri, OBIS 1-1:5.8.0
        Q2_EXPORT_LAGGING = 1,  // +Rc, OBIS 1-1:6.8.0
        Q3_EXPORT_LEADING = 2,  // -Ri, OBIS 1-1:7.8.0
        Q4_IMPORT_LEADING = 3,  // -Rc, OBIS 1-1:8.8.0
    };

    void set_import_w(uint32_t watts) { import_w_ = watts; }
    void set_export_w(uint32_t watts) { export_w_ = watts; }

    // pf in [0.0, 1.0]. Lagging (inductive) is the default because that is what
    // an ordinary household load is. Out-of-range values are clamped rather than
    // rejected — the console rejects them first (FR-CLI-05), so anything
    // arriving here is a programming error, not user input.
    void set_power_factor(float pf, bool lagging = true);

    // Integrate over elapsed_ms and advance every accumulator (FR-MDL-05).
    //
    // Accumulators are milli-unit-seconds in uint64 (FR-MDL-04). Watts × ms is
    // exactly milliwatt-seconds, so active energy accrues with no rounding at
    // all — the truncation that would lose 7 % of a 777 W load, and 42 % of a
    // 300 W one, simply cannot occur.
    void tick(uint32_t elapsed_ms);

    [[nodiscard]] uint32_t import_w() const { return import_w_; }
    [[nodiscard]] uint32_t export_w() const { return export_w_; }
    [[nodiscard]] float power_factor() const { return power_factor_; }
    [[nodiscard]] bool lagging() const { return lagging_; }

    [[nodiscard]] uint32_t import_wh() const { return to_whole_units(active_[0]); }
    [[nodiscard]] uint32_t export_wh() const { return to_whole_units(active_[1]); }
    [[nodiscard]] uint32_t reactive_varh(Quadrant q) const { return to_whole_units(reactive_[q]); }

    // Balanced three phases, so one value serves all three (FSD A-02).
    [[nodiscard]] uint32_t current_ma() const;

  private:
    // milli-units × seconds -> whole units. 1 Wh = 3 600 000 mW·s.
    static uint32_t to_whole_units(uint64_t milli_unit_seconds);

    uint32_t import_w_ = 0;
    uint32_t export_w_ = 0;
    float power_factor_ = 1.0F;
    bool lagging_ = true;

    uint64_t active_[2] = {0, 0};     // import, export
    uint64_t reactive_[4] = {0, 0, 0, 0};  // by Quadrant
};

}  // namespace e450

// Host-tier tests — FSD §12.
//
// Every case is registered separately in CMakeLists.txt under the requirement it
// discharges, so a red build names the property that broke.
//
// The oracle in `decode` is dlms_parser v2.1.0 — a DLMS decoder we did not
// write. That is the whole design: if an independent implementation cannot read
// our bytes on a laptop, nothing will read them on the wire either, and we find
// out in seconds instead of on the bench. Note what this proves and what it does
// not: it proves the output is *decodable*, not that it is *faithful*. A tolerant
// decoder would accept telegrams a real E450 never emits.

#include <dlms_parser/dlms_parser.h>
#include <e450/directives.h>
#include <e450/ebl_ci1.h>
#include <e450/emission.h>
#include <e450/faults.h>
#include <e450/modes.h>
#include <e450/rng.h>
#include <e450/telegram.h>
#include <e450/value_model.h>

#include <cinttypes>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        ++failures;
    }
}

void check_eq(double actual, double expected, const char *what, double tol = 0.5) {
    if (std::fabs(actual - expected) > tol) {
        std::printf("  FAIL  %s: expected %.3f, got %.3f\n", what, expected, actual);
        ++failures;
    }
}

e450::TelegramSpec make_spec(const std::vector<e450::TelegramValue> &values, uint32_t invoke_id,
                             const e450::DateTime &when) {
    e450::TelegramSpec spec;
    spec.objects = e450::ebl::ci1_objects();
    spec.values = values;
    spec.invoke_id = invoke_id;
    spec.timestamp = when;
    return spec;
}

// Encode one CI1 telegram from a model snapshot.
std::vector<uint8_t> encode(const e450::ValueModel &model, uint32_t invoke_id = 0,
                            std::size_t block_payload = 128) {
    const std::vector<e450::TelegramValue> values = e450::ebl::ci1_values(model);
    e450::WireConfig wire;
    wire.block_payload = block_payload;
    return e450::encode_telegram(make_spec(values, invoke_id, e450::DateTime{}), wire);
}

// Run the bytes through the independent decoder and collect every numeric capture,
// keyed by OBIS code in dlms_parser's "a.b.c.d.e.f" spelling.
std::map<std::string, double> decode(std::vector<uint8_t> bytes, std::size_t *count = nullptr) {
    std::map<std::string, double> values;
    dlms_parser::DlmsParser parser([&values](const dlms_parser::AxdrCapture &capture) {
        if (!capture.is_numeric()) return;
        char buffer[32];
        values[std::string(capture.obis.to_string(buffer))] =
            capture.value_as_float_with_scaler_applied();
    });
    parser.load_default_patterns();

    const dlms_parser::ParseResult result = parser.parse(bytes);
    if (count != nullptr) *count = result.count;
    return values;
}

constexpr const char *OBIS_P_IMPORT = "1.0.1.7.0.255";
constexpr const char *OBIS_P_EXPORT = "1.0.2.7.0.255";
constexpr const char *OBIS_E_IMPORT = "1.1.1.8.0.255";
constexpr const char *OBIS_E_EXPORT = "1.1.2.8.0.255";
constexpr const char *OBIS_I_L1 = "1.0.31.7.0.255";
constexpr const char *OBIS_I_L3 = "1.0.71.7.0.255";

double at(const std::map<std::string, double> &values, const char *obis) {
    const auto it = values.find(obis);
    if (it == values.end()) {
        std::printf("  FAIL  OBIS %s absent from decode\n", obis);
        ++failures;
        return -1.0;
    }
    return it->second;
}

// ---------------------------------------------------------------- cases

// FR-ENC-01..04: an independent decoder reads our telegram and recovers the model.
void case_decode() {
    e450::ValueModel model;
    model.set_import_w(1500);
    model.tick(3'600'000);  // one hour

    std::size_t count = 0;
    const std::map<std::string, double> values = decode(encode(model), &count);

    check(count > 0, "parser reported no matched objects");
    check_eq(at(values, OBIS_P_IMPORT), 1500, "active power import");
    check_eq(at(values, OBIS_P_EXPORT), 0, "active power export");
    check_eq(at(values, OBIS_E_IMPORT), 1500, "active energy import after 1 h at 1500 W");
    check_eq(at(values, OBIS_E_EXPORT), 0, "active energy export");
    check_eq(at(values, OBIS_I_L1), 2173, "current L1");
    check_eq(at(values, OBIS_I_L3), 2173, "current L3");
}

// FR-ENC-05: the invoke id counts telegrams. Anything that dedupes on it would
// otherwise treat every burst as a repeat of the first.
void case_invoke_id() {
    const e450::ValueModel model;
    const std::vector<e450::TelegramValue> values = e450::ebl::ci1_values(model);
    for (uint32_t id = 0; id < 3; ++id) {
        const std::vector<uint8_t> apdu =
            e450::encode_apdu(make_spec(values, id, e450::DateTime{}));
        const uint32_t encoded = (static_cast<uint32_t>(apdu[1]) << 24) |
                                 (static_cast<uint32_t>(apdu[2]) << 16) |
                                 (static_cast<uint32_t>(apdu[3]) << 8) | apdu[4];
        check(apdu[0] == 0x0F, "APDU tag is DataNotification");
        check(encoded == id, "invoke id round-trips");
    }
}

// FR-ENC-06/07: streaming GBT, numbered from 1, only the final block flagged.
void case_gbt() {
    e450::ValueModel model;
    model.set_import_w(1500);
    const std::vector<e450::TelegramValue> values = e450::ebl::ci1_values(model);
    const std::vector<uint8_t> apdu = e450::encode_apdu(make_spec(values, 0, e450::DateTime{}));

    const std::size_t block_payload = 128;
    const auto blocks = e450::segment_gbt(apdu, block_payload);
    const std::size_t expected = (apdu.size() + block_payload - 1) / block_payload;

    check(blocks.size() == expected, "block count matches APDU size");
    check(blocks.size() > 1, "a CI1 telegram does not fit one block");
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const bool last = (i + 1 == blocks.size());
        check(blocks[i][0] == 0xE0, "GBT tag");
        check(blocks[i][1] == (last ? 0xC0 : 0x40), "block control: last flag only on the last");
        check(blocks[i][2] == 0 && blocks[i][3] == i + 1, "block numbered from 1");
        check(blocks[i][4] == 0 && blocks[i][5] == 0, "acknowledged-block field zero");
    }
}

// FR-ENC-07: the split policy is ours (FSD A-03), so it must not change meaning.
void case_block_size() {
    e450::ValueModel model;
    model.set_import_w(2400);
    model.tick(3'600'000);

    std::size_t count_128 = 0;
    std::size_t count_64 = 0;
    const auto with_128 = decode(encode(model, 0, 128), &count_128);
    const auto with_64 = decode(encode(model, 0, 64), &count_64);

    check(count_64 > 0, "64-byte blocks still parse");
    check(with_128 == with_64, "decoded values identical across block sizes");
    check(encode(model, 0, 64).size() > encode(model, 0, 128).size(),
          "smaller blocks cost more frames");
}

// FR-ENC-08..10: frame boundaries and both checksums, recomputed independently
// of the encoder rather than by calling back into it.
void case_hdlc() {
    e450::ValueModel model;
    model.set_import_w(1000);
    const std::vector<uint8_t> wire = encode(model);

    auto crc = [](const uint8_t *data, std::size_t len) {
        uint16_t c = 0xFFFF;
        for (std::size_t i = 0; i < len; ++i) {
            c ^= data[i];
            for (int b = 0; b < 8; ++b) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
        }
        return uint16_t(c ^ 0xFFFF);
    };

    std::size_t pos = 0;
    std::size_t frames = 0;
    while (pos < wire.size()) {
        check(wire[pos] == 0x7E, "frame opens with a flag");
        check((wire[pos + 1] & 0xF0) == 0xA0, "HDLC type 3 format byte");

        const std::size_t length = ((wire[pos + 1] & 0x07) << 8) | wire[pos + 2];
        const std::size_t close = pos + 1 + length;
        check(close < wire.size() && wire[close] == 0x7E,
              "closing flag sits at start + 1 + length");

        check(wire[pos + 3] == 0xCE && wire[pos + 4] == 0xFF, "two-byte destination CE FF");
        check(wire[pos + 5] == 0x03, "source 03");
        check(wire[pos + 6] == 0x13, "control 13");

        const uint16_t hcs = crc(&wire[pos + 1], 6);
        check(wire[pos + 7] == (hcs & 0xFF) && wire[pos + 8] == (hcs >> 8), "HCS little-endian");

        // The FCS covers the format byte through the last information byte. The
        // length counts the format byte through the FCS, so that is length - 2
        // bytes — the two FCS bytes themselves are not covered.
        const uint16_t fcs = crc(&wire[pos + 1], length - 2);
        check(wire[close - 2] == (fcs & 0xFF) && wire[close - 1] == (fcs >> 8),
              "FCS little-endian");

        // LLC in the first frame only.
        const bool llc = wire[pos + 9] == 0xE6 && wire[pos + 10] == 0xE7 && wire[pos + 11] == 0x00;
        check(llc == (frames == 0), "LLC header appears in the first frame only");

        ++frames;
        pos = close + 1;
    }
    check(frames > 1, "telegram spans several frames");
}

// FR-MDL-03/05: energy is the integral of power, tick by tick.
void case_energy() {
    e450::ValueModel model;
    model.set_import_w(300);
    uint32_t previous = 0;
    for (int i = 0; i < 720; ++i) {  // 720 x 5 s = 1 h
        model.tick(5000);
        check(model.import_wh() >= previous, "import counter never decreases");
        previous = model.import_wh();
    }
    check_eq(model.import_wh(), 300, "300 W for one hour is 300 Wh");
}

// FR-MDL-04: the failure this guards is silent. Integrating in whole watt-hours
// would truncate 1.079 Wh to 1 every tick and lose 7 % of a 777 W load for ever.
void case_truncation() {
    e450::ValueModel model;
    model.set_import_w(777);
    for (int i = 0; i < 720; ++i) model.tick(5000);
    check_eq(model.import_wh(), 777, "777 W for one hour is 777 Wh, not 720");
}

// FR-MDL-07.
void case_currents() {
    e450::ValueModel model;
    model.set_import_w(1587);
    check_eq(model.current_ma(), 2300, "1587 W over three 230 V phases is 2300 mA");
}

// FR-MDL-09: at unity power factor there is no reactive power, so all four
// quadrant counters must hold — not merely stay small.
void case_reactive() {
    e450::ValueModel model;
    model.set_import_w(2000);
    model.set_export_w(500);
    model.set_power_factor(1.0F);
    for (int i = 0; i < 100; ++i) model.tick(5000);
    for (int q = 0; q < 4; ++q) {
        check(model.reactive_varh(static_cast<e450::ValueModel::Quadrant>(q)) == 0,
              "unity power factor accumulates no reactive energy");
    }
}

// FR-MDL-10: import and export are separate registers. Exporting must not run
// the import counter backwards, and a decoder cannot tell a reset from a real
// zero.
void case_monotonic() {
    e450::ValueModel model;
    model.set_import_w(1000);
    for (int i = 0; i < 10; ++i) model.tick(5000);
    const uint32_t imported = model.import_wh();
    check(imported > 0, "import accumulated");

    model.set_import_w(0);
    model.set_export_w(1000);
    for (int i = 0; i < 10; ++i) model.tick(5000);

    check(model.import_wh() == imported, "import holds while exporting");
    check(model.export_wh() > 0, "export accumulates");
}

// FR-CLK-04: identical state must produce identical bytes, or nothing
// downstream can ever be byte-compared.
void case_determinism() {
    e450::ValueModel model;
    model.set_import_w(1234);
    model.tick(60'000);

    e450::DateTime when;
    when.year = 2026;
    when.month = 8;
    when.day = 4;
    when.hour = 17;
    when.minute = 0;
    when.second = 5;

    const std::vector<e450::TelegramValue> values = e450::ebl::ci1_values(model);
    const std::vector<uint8_t> first = e450::encode_telegram(make_spec(values, 7, when));
    const std::vector<uint8_t> second = e450::encode_telegram(make_spec(values, 7, when));
    check(first == second, "same state, same bytes");
}

// FR-SCH-08 — the telegram must leave a silent gap long enough for the
// transmission to be called finished.
//
// This is a host test because the arithmetic is fully determined here: the CI1
// list is fixed and every value is a fixed-width encoding, so the telegram is
// the same size whatever the model holds. If it ever stops fitting — a longer
// object list, a second push setup, a wider value — the failure is a merged
// burst at the far end, which presents as a decode fault downstream and gets
// debugged in the wrong place. Catching it in ctest costs seconds instead.
void case_silence() {
    e450::ValueModel model;
    e450::DateTime when;
    when.year = 2026;
    when.month = 8;
    when.day = 4;
    when.hour = 17;
    when.minute = 0;
    when.second = 5;

    // Worst case for size is not the empty model: exercise values that occupy
    // every byte of their fixed-width encodings.
    model.set_import_w(0xFFFFFFFF);
    model.set_export_w(0xFFFFFFFF);
    model.tick(3600u * 1000u);

    const std::vector<uint8_t> wire =
        e450::encode_telegram(make_spec(e450::ebl::ci1_values(model), 0xFFFFFFFF, when));

    const uint32_t period_ms = 5000;
    const int32_t silence = e450::silence_ms(wire.size(), period_ms);
    std::printf("  %zu bytes, %" PRIu32 " ms on the wire, %" PRId32 " ms silent in %u ms\n",
                wire.size(), e450::wire_time_ms(wire.size()), silence, period_ms);

    check(silence >= e450::MIN_SILENCE_MS, "5 s period leaves the 3 s target gap");
    check(silence >= e450::BURST_TIMEOUT_MS, "5 s period clears the 2 s burst timeout");

    // The rounding must be conservative, or a telegram that just misses passes.
    check(e450::wire_time_ms(1) == 5, "one byte is 11 bits = 4.58 ms, rounded up");
    check(e450::wire_time_ms(0) == 0, "no bytes, no time");
}

// FR-MOD-03 — mode 2 decodes, and fits in one HDLC frame.
//
// The frame count is the property that makes this mode worth having. Mode 3
// spans three frames and needs multi-block GBT reassembly; if mode 2 decodes and
// mode 3 does not, multi-frame reassembly is the fault and nothing else is.
// That inference only holds while mode 2 really is a single frame, so it is
// asserted rather than assumed.
void case_mode_simple() {
    const std::vector<e450::TelegramValue> fixed = e450::simple::values();
    e450::TelegramSpec spec;
    spec.objects = e450::simple::objects();
    spec.bind_values(fixed);
    spec.invoke_id = 1;
    spec.timestamp = e450::DateTime{};

    const std::vector<uint8_t> wire = e450::encode_telegram(spec);

    std::size_t frames = 0;
    for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
        if (wire[i] == 0x7E && wire[i + 1] == 0xA0) ++frames;
    }
    std::printf("  %zu bytes, %zu frame(s)\n", wire.size(), frames);
    check(frames == 1, "mode 2 is a single HDLC frame");

    std::size_t count = 0;
    const std::map<std::string, double> values = decode(wire, &count);
    check(count > 0, "parser reported no matched objects");
    check_eq(at(values, OBIS_P_IMPORT), e450::simple::ACTIVE_POWER_W, "fixed active power");
    check_eq(at(values, OBIS_E_IMPORT), e450::simple::ACTIVE_ENERGY_WH, "fixed active energy");
}

// FR-MOD-04 — mode 3 varies, stays physically consistent, and repeats exactly.
void case_mode_random() {
    // Repeatable: the same seed must give the same load sequence, or FR-CLK-04
    // (two runs, identical bytes) cannot hold once mode 3 is the default.
    e450::Rng a{e450::MODE3_SEED};
    e450::Rng b{e450::MODE3_SEED};
    e450::ValueModel first;
    e450::ValueModel second;
    for (int i = 0; i < 20; ++i) {
        e450::apply_random_load(first, a);
        e450::apply_random_load(second, b);
        check(first.import_w() == second.import_w() && first.export_w() == second.export_w(),
              "same seed, same sequence");
    }

    // Varies: a mode whose whole purpose is to stop a decoder passing on a
    // constant must not itself emit a constant.
    e450::Rng rng{e450::MODE3_SEED};
    e450::ValueModel model;
    std::map<uint32_t, int> seen;
    for (int i = 0; i < 50; ++i) {
        e450::apply_random_load(model, rng);
        seen[model.import_w() + model.export_w()]++;
    }
    std::printf("  %zu distinct loads in 50 draws\n", seen.size());
    check(seen.size() > 25, "mode 3 values actually vary");

    // Consistent: the load is chosen before the period is integrated, so the
    // energy a telegram reports is exactly the power it reports times the
    // period. This is the arithmetic a decode can be checked against.
    e450::Rng rng2{e450::MODE3_SEED};
    e450::ValueModel m2;
    uint32_t expected_mwh_s = 0;
    for (int i = 0; i < 12; ++i) {
        e450::apply_random_load(m2, rng2);
        m2.tick(5000);
        expected_mwh_s += m2.import_w() * 5000;
    }
    check_eq(m2.import_wh(), expected_mwh_s / 3'600'000u, "energy is the integral of the load");

    // Import and export never both flow: a real meter measures one direction at
    // a time, and a decoder is entitled to assume it.
    e450::Rng rng3{e450::MODE3_SEED};
    e450::ValueModel m3;
    for (int i = 0; i < 100; ++i) {
        e450::apply_random_load(m3, rng3);
        check(m3.import_w() == 0 || m3.export_w() == 0, "only one direction at a time");
    }
}


// ------------------------------------------------- phase 2 directives (§14)

// Walk a byte stream as HDLC frames, reporting each frame's length and whether
// its FCS is right. Written here rather than reusing the encoder, so a fault
// test cannot be fooled by the same mistake twice.
struct FrameInfo {
    std::size_t offset;
    std::size_t length;
    bool fcs_ok;
    bool closed;
};

uint16_t x25(const uint8_t *data, std::size_t len) {
    uint16_t c = 0xFFFF;
    for (std::size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int b = 0; b < 8; ++b) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
    }
    return uint16_t(c ^ 0xFFFF);
}

std::vector<FrameInfo> walk(const std::vector<uint8_t> &wire) {
    std::vector<FrameInfo> out;
    std::size_t pos = 0;
    while (pos + 3 < wire.size()) {
        if (wire[pos] != 0x7E) { ++pos; continue; }
        const std::size_t len = (std::size_t(wire[pos + 1] & 0x07) << 8) | wire[pos + 2];
        if (len < 10 || pos + 1 + len > wire.size()) break;
        const uint16_t want = uint16_t(wire[pos + len - 1] | (wire[pos + len] << 8));
        const uint16_t got = x25(&wire[pos + 1], len - 2);
        out.push_back({pos, len + 2, want == got, pos + 1 + len < wire.size() &&
                                                      wire[pos + 1 + len] == 0x7E});
        pos += len + 2;
    }
    return out;
}

std::vector<std::vector<uint8_t>> fault_frames() {
    const e450::EmissionConfig cfg;
    const e450::FaultSpec none;
    const e450::Emission plan = e450::plan_e450(e450::fixed_model(), cfg, none);
    return e450::encode_telegram_frames(plan.spec(1, e450::DateTime{}));
}

// FR-FLT-01 — the load a fault telegram carries is fixed.
void case_fault_fixed_values() {
    const e450::ValueModel a = e450::fixed_model();
    const e450::ValueModel b = e450::fixed_model();
    check(a.import_w() == b.import_w() && a.import_wh() == b.import_wh(),
          "fixed_model is the same every call");
    check(a.import_w() == 777, "fixed load is mode 2's 777 W");
    check(a.import_wh() == 777, "one hour at 777 W is 777 Wh");
    std::printf("  fixed: %u W, %u Wh\n", unsigned(a.import_w()), unsigned(a.import_wh()));
}

// FR-FLT-02 — frame n's FCS is wrong; every other frame is untouched.
void case_fault_fcs() {
    const auto frames = fault_frames();
    check(frames.size() >= 3, "the e450 telegram spans several frames");

    e450::FaultSpec f;
    f.kind = e450::Fault::Fcs;
    f.arg = 2;
    const std::vector<uint8_t> wire = e450::apply_wire_fault(frames, f);
    const std::vector<uint8_t> clean = e450::apply_wire_fault(frames, e450::FaultSpec{});

    check(wire.size() == clean.size(), "corrupting an FCS does not change the length");
    const auto walked = walk(wire);
    check(walked.size() == frames.size(), "every frame is still present");
    for (std::size_t i = 0; i < walked.size(); ++i) {
        const bool should_fail = (i == 1);
        check(walked[i].fcs_ok != should_fail,
              should_fail ? "frame 2's FCS is wrong" : "other frames' FCS are right");
    }
}

// FR-FLT-03 — frame n is cut inside its information field, with no closing flag.
void case_fault_truncate() {
    const auto frames = fault_frames();
    e450::FaultSpec f;
    f.kind = e450::Fault::Truncate;
    f.arg = 1;
    const std::vector<uint8_t> wire = e450::apply_wire_fault(frames, f);

    check(wire.size() < e450::apply_wire_fault(frames, e450::FaultSpec{}).size(),
          "truncation removes bytes");
    // The declared length of frame 1 now exceeds what is actually there, which
    // is the condition a receiver has to reject rather than half-accept.
    const std::size_t declared = (std::size_t(wire[1] & 0x07) << 8) | wire[2];
    std::size_t to_next_flag = 1;
    while (to_next_flag < wire.size() && wire[to_next_flag] != 0x7E) ++to_next_flag;
    check(declared + 1 > to_next_flag, "frame 1 claims more bytes than it carries");
}

// FR-FLT-04 — block k is never sent, so the APDU cannot be completed.
void case_fault_drop_block() {
    const auto frames = fault_frames();
    e450::FaultSpec f;
    f.kind = e450::Fault::DropBlock;
    f.arg = 2;
    const std::vector<uint8_t> wire = e450::apply_wire_fault(frames, f);

    const auto walked = walk(wire);
    check(walked.size() == frames.size() - 1, "one frame fewer");
    for (const FrameInfo &fi : walked) check(fi.fcs_ok, "the surviving frames are intact");

    // The GBT block numbers must show the gap — that is what makes the APDU
    // provably incomplete rather than merely short. Block number is the two
    // bytes after the 0xE0 tag and control byte in the first frame's payload.
    std::vector<unsigned> blocks;
    for (const FrameInfo &fi : walked) {
        for (std::size_t i = fi.offset; i + 5 < fi.offset + fi.length; ++i) {
            if (wire[i] == 0xE0 && (wire[i + 1] == 0x40 || wire[i + 1] == 0xC0)) {
                blocks.push_back(unsigned(wire[i + 3]));
                break;
            }
        }
    }
    std::printf("  blocks on the wire:");
    for (unsigned b : blocks) std::printf(" %u", b);
    std::printf("\n");
    check(blocks.size() == frames.size() - 1, "one block fewer on the wire");
    check(std::find(blocks.begin(), blocks.end(), 2u) == blocks.end(), "block 2 is absent");

    // What a decoder does with the remains is its business, not ours. Recorded
    // rather than asserted: this oracle is permissive and salvages captures from
    // an incomplete APDU, which is exactly the behaviour the directive exists to
    // provoke a test for downstream.
    std::size_t count = 0;
    decode(wire, &count);
    std::printf("  a permissive decoder still matched %zu capture(s) from the remains\n", count);
}

// FR-FLT-05 — transmission begins partway through frame 1.
void case_fault_start_mid_frame() {
    const auto frames = fault_frames();
    e450::FaultSpec f;
    f.kind = e450::Fault::StartMidFrame;
    const std::vector<uint8_t> wire = e450::apply_wire_fault(frames, f);
    const std::vector<uint8_t> clean = e450::apply_wire_fault(frames, e450::FaultSpec{});

    check(wire.size() == clean.size() - e450::START_MID_FRAME_SKIP, "the head is missing");
    check(wire[0] != 0x7E, "the stream does not begin on a flag");
    // Frames 2 onward are byte-identical, so anything that resynchronises on the
    // next flag recovers them in full.
    check(std::equal(frames[1].begin(), frames[1].end(),
                     wire.begin() + std::ptrdiff_t(frames[0].size() - e450::START_MID_FRAME_SKIP)),
          "frame 2 survives intact");
}

// FR-FLT-06 — one register appears twice, with different values.
void case_fault_duplicate_register() {
    e450::FaultSpec f;
    f.kind = e450::Fault::DuplicateRegister;
    f.obis = {1, 1, 1, 8, 0, 0xFF};
    const e450::EmissionConfig cfg;
    const e450::Emission plan = e450::plan_simple(cfg, f);

    std::size_t seen = 0;
    for (const e450::CaptureObject &o : plan.objects) {
        if (o.obis == f.obis) ++seen;
    }
    check(seen == 2, "the register is listed twice");
    check(plan.objects.size() == plan.values.size(), "one value per object");

    // The two values must differ, or "kept the first" and "kept the last" are
    // the same observation and the fault proves nothing.
    uint32_t first = 0, second = 0, hits = 0;
    for (std::size_t i = 0; i < plan.objects.size(); ++i) {
        if (plan.objects[i].obis != f.obis) continue;
        const uint32_t v = plan.values[i].u32;
        (hits++ == 0 ? first : second) = v;
    }
    std::printf("  duplicate values: %u then %u\n", unsigned(first), unsigned(second));
    check(first != second, "the duplicate carries a different value");
}

// FR-FLT-07 — a burst of bytes with no framing in it at all.
void case_fault_noise() {
    const std::vector<uint8_t> n = e450::noise_bytes(64);
    check(n.size() == 64, "the requested number of bytes");
    for (uint8_t b : n) check(b != 0x7E, "no byte can be mistaken for a flag");
    check(e450::noise_bytes(64) == n, "noise is deterministic");

    std::size_t count = 0;
    decode(n, &count);
    check(count == 0, "noise decodes to nothing");
}

// FR-FLT-10/11 — which identity object appears, and how long the serial is.
void case_directive_identity() {
    const e450::FaultSpec none;
    const std::array<uint8_t, 6> devid{0, 0, 96, 1, 0, 0xFF};
    const std::array<uint8_t, 6> ldn{0, 0, 42, 0, 0, 0xFF};

    auto has = [&](const e450::Emission &e, const std::array<uint8_t, 6> &obis) {
        for (const e450::CaptureObject &o : e.objects) {
            if (o.obis == obis) return true;
        }
        return false;
    };

    e450::EmissionConfig cfg;
    cfg.identity = e450::Identity::None;
    const e450::Emission a = e450::plan_simple(cfg, none);
    check(!has(a, devid) && !has(a, ldn), "identity none carries neither object");

    cfg.identity = e450::Identity::DevId;
    cfg.serial_chars = 8;
    const e450::Emission b = e450::plan_simple(cfg, none);
    check(has(b, devid) && !has(b, ldn), "identity devid carries 0-0:96.1.0");

    cfg.identity = e450::Identity::Ldn;
    const e450::Emission c = e450::plan_simple(cfg, none);
    check(has(c, ldn) && !has(c, devid), "identity ldn carries 0-0:42.0.0");

    // FR-FLT-11: the serial's length is what changes, not its digits.
    std::size_t len8 = 0, len16 = 0;
    cfg.identity = e450::Identity::DevId;
    cfg.serial_chars = 8;
    for (const e450::TelegramValue &v : e450::plan_simple(cfg, none).values) {
        if (v.kind == e450::TelegramValue::Kind::OctetString) len8 = v.octets.size();
    }
    cfg.serial_chars = 16;
    for (const e450::TelegramValue &v : e450::plan_simple(cfg, none).values) {
        if (v.kind == e450::TelegramValue::Kind::OctetString) len16 = v.octets.size();
    }
    std::printf("  serial lengths: %zu and %zu\n", len8, len16);
    check(len8 == 8 && len16 == 16, "both serial forms available without a rebuild");
}

// FR-FLT-12 — a cumulative register set directly, above the float boundary.
//
// 16 777 217 is 2^24 + 1: the first integer a 32-bit float cannot represent.
// The encoder must carry it exactly. The oracle here returns a float, so it
// *cannot* confirm that end to end — and this test says so out loud rather than
// asserting something weaker and calling it proof.
void case_directive_energy() {
    e450::EmissionConfig cfg;
    cfg.energy_set = true;
    cfg.energy_wh = 16'777'217u;
    const e450::Emission plan = e450::plan_e450(e450::fixed_model(), cfg, e450::FaultSpec{});

    // Position 5 of Appendix A is 1-1:1.8.0 — asserted, not assumed.
    const std::array<uint8_t, 6> energy_obis{1, 1, 1, 8, 0, 0xFF};
    check(plan.objects[4].obis == energy_obis, "the override lands on 1-1:1.8.0");
    check(plan.values[4].u32 == 16'777'217u, "the exact value is carried");

    // And it survives into the bytes as a 4-byte unsigned, not a float.
    const std::vector<uint8_t> apdu = e450::encode_apdu(plan.spec(0, e450::DateTime{}));
    const uint8_t want[5] = {0x06, 0x01, 0x00, 0x00, 0x01};
    check(std::search(apdu.begin(), apdu.end(), want, want + 5) != apdu.end(),
          "06 01 00 00 01 appears in the APDU");

    const std::map<std::string, double> values = decode(e450::encode_telegram(
        plan.spec(0, e450::DateTime{})));
    const double seen = values.count(OBIS_E_IMPORT) ? values.at(OBIS_E_IMPORT) : -1;
    std::printf("  sent 16777217, a float oracle reports %.0f%s\n", seen,
                seen == 16'777'216.0 ? "  <- quantised, as expected" : "");
}

// FR-FLT-13 — a non-zero scaler, applied exactly once.
void case_directive_scaler() {
    for (const int8_t scaler : {int8_t(0), int8_t(-2), int8_t(2)}) {
        e450::EmissionConfig cfg;
        cfg.scaler = scaler;
        const e450::Emission plan = e450::plan_simple(cfg, e450::FaultSpec{});
        const std::map<std::string, double> values =
            decode(e450::encode_telegram(plan.spec(0, e450::DateTime{})));

        const double got = values.count(OBIS_E_IMPORT) ? values.at(OBIS_E_IMPORT) : -1;
        const double want = scaler == 0 ? 12345.0 : (scaler == -2 ? 123.45 : 1'234'500.0);
        std::printf("  scaler %d -> %.2f (want %.2f)\n", scaler, got, want);
        check_eq(got, want, "scaling applied exactly once", want * 1e-6 + 0.001);
    }
}

// FR-FLT-02/04 sanity: a fault-free emission is byte-identical to encode_telegram.
void case_fault_none_is_transparent() {
    const auto frames = fault_frames();
    const e450::Emission plan =
        e450::plan_e450(e450::fixed_model(), e450::EmissionConfig{}, e450::FaultSpec{});
    check(e450::apply_wire_fault(frames, e450::FaultSpec{}) ==
              e450::encode_telegram(plan.spec(1, e450::DateTime{})),
          "no fault means the same bytes as the plain encoder");
}


// FR-MOD-06 — the counted ramp: the first byte received is the loss count.
void case_mode_ramp() {
    const std::vector<uint8_t> r = e450::ramp_bytes(381);
    check(r.size() == 381, "the requested length");
    check(r[0] == 0x00, "every burst starts at zero, or the count means nothing");
    for (std::size_t i = 0; i < r.size(); ++i) {
        check(r[i] == uint8_t(i & 0xFF), "byte i is i mod 256");
    }

    // The property the mode exists for, stated as the test: drop the first k
    // bytes and the first byte you now see *is* k.
    for (const std::size_t lost : {std::size_t(1), std::size_t(25), std::size_t(255)}) {
        const std::vector<uint8_t> received(r.begin() + std::ptrdiff_t(lost), r.end());
        check(received[0] == uint8_t(lost), "first byte received equals bytes lost");
    }
    std::printf("  381 bytes, wraps at 256, loss readable up to 255\n");

    // Same duration as a telegram, which is what makes it comparable.
    check(e450::wire_time_ms(r.size()) == e450::wire_time_ms(381),
          "same wire time as the telegram it stands in for");
}

// FR-FLT-16 — a preamble ahead of the first frame, and what it costs.
//
// The directive exists to answer "is a lead-in a viable mitigation?", so this
// case is written to *record the answer*, not to assume one. It is a
// characterisation test: if the oracle's behaviour ever changes, it fails, and
// whoever reads it learns what changed.
void case_preamble() {
    const auto frames = fault_frames();
    const std::vector<uint8_t> plain = e450::apply_wire_fault(frames, e450::FaultSpec{}, 0);

    // Structural: the burst grows by exactly the preamble and the telegram
    // behind it is untouched. This part is ours and must always hold.
    for (const uint8_t filler : {uint8_t(0x7E), uint8_t(0x00)}) {
        const std::vector<uint8_t> led =
            e450::apply_wire_fault(frames, e450::FaultSpec{}, 32, filler);
        check(led.size() == plain.size() + 32, "the burst grows by exactly the preamble");
        for (int i = 0; i < 32; ++i) check(led[i] == filler, "filler is the byte asked for");
        check(std::equal(plain.begin(), plain.end(), led.begin() + 32),
              "the telegram after the preamble is byte-identical");
    }

    // Behavioural: what a decoder makes of it. Recorded, because this is the
    // finding the directive was built to produce.
    std::size_t bare = 0, one_flag = 0, many_flags = 0, non_flag = 0;
    decode(plain, &bare);
    decode(e450::apply_wire_fault(frames, e450::FaultSpec{}, 1, 0x7E), &one_flag);
    decode(e450::apply_wire_fault(frames, e450::FaultSpec{}, 32, 0x7E), &many_flags);
    decode(e450::apply_wire_fault(frames, e450::FaultSpec{}, 32, 0x00), &non_flag);
    std::printf("  captures: bare=%zu  1x7E=%zu  32x7E=%zu  32x00=%zu\n", bare, one_flag,
                many_flags, non_flag);

    check(bare > 0, "the telegram decodes with no preamble at all");
    check(one_flag == 0 && many_flags == 0,
          "recorded: this decoder rejects a telegram behind ANY flag fill, even one byte");
    check(non_flag > 0 && non_flag < bare,
          "recorded: non-flag fill costs the first frame and no more");
}

// FR-FLT-16 with FR-FLT-05 — the mitigation, tested as a mitigation.
void case_preamble_absorbs_truncation() {
    const auto frames = fault_frames();
    e450::FaultSpec cut;
    cut.kind = e450::Fault::StartMidFrame;

    // Without a lead-in, the cut lands inside frame 1 and the stream no longer
    // starts on a flag.
    const std::vector<uint8_t> bare = e450::apply_wire_fault(frames, cut, 0);
    check(bare[0] != 0x7E, "with no preamble the cut eats into frame 1");

    // With a lead-in at least as long as the cut, frame 1 arrives whole — which
    // is the entire argument for emitting a preamble at all.
    const std::vector<uint8_t> shielded =
        e450::apply_wire_fault(frames, cut, e450::START_MID_FRAME_SKIP, 0x00);
    check(shielded.size() == e450::apply_wire_fault(frames, e450::FaultSpec{}, 0).size(),
          "the preamble absorbed the cut exactly");
    check(std::equal(frames[0].begin(), frames[0].end(), shielded.begin()),
          "frame 1 survives intact behind the preamble");

    std::size_t bare_count = 0, shielded_count = 0;
    decode(bare, &bare_count);
    decode(shielded, &shielded_count);
    std::printf("  truncated start: %zu captures bare, %zu behind a %zu-byte preamble\n",
                bare_count, shielded_count, e450::START_MID_FRAME_SKIP);
    check(shielded_count >= bare_count, "the preamble does not make things worse");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_encode <case>\n");
        return 2;
    }
    const std::string name = argv[1];

    if (name == "decode") case_decode();
    else if (name == "invoke_id") case_invoke_id();
    else if (name == "gbt") case_gbt();
    else if (name == "block_size") case_block_size();
    else if (name == "hdlc") case_hdlc();
    else if (name == "energy") case_energy();
    else if (name == "truncation") case_truncation();
    else if (name == "currents") case_currents();
    else if (name == "reactive") case_reactive();
    else if (name == "monotonic") case_monotonic();
    else if (name == "determinism") case_determinism();
    else if (name == "silence") case_silence();
    else if (name == "mode_simple") case_mode_simple();
    else if (name == "mode_random") case_mode_random();
    else if (name == "fault_fixed") case_fault_fixed_values();
    else if (name == "fault_fcs") case_fault_fcs();
    else if (name == "fault_truncate") case_fault_truncate();
    else if (name == "fault_drop_block") case_fault_drop_block();
    else if (name == "fault_start_mid") case_fault_start_mid_frame();
    else if (name == "fault_duplicate") case_fault_duplicate_register();
    else if (name == "fault_noise") case_fault_noise();
    else if (name == "fault_none") case_fault_none_is_transparent();
    else if (name == "identity") case_directive_identity();
    else if (name == "energy") case_directive_energy();
    else if (name == "scaler") case_directive_scaler();
    else if (name == "ramp") case_mode_ramp();
    else if (name == "preamble") case_preamble();
    else if (name == "preamble_cut") case_preamble_absorbs_truncation();
    else {
        std::fprintf(stderr, "unknown case: %s\n", name.c_str());
        return 2;
    }

    std::printf("%s: %d failure(s)\n", name.c_str(), failures);
    return failures == 0 ? 0 : 1;
}

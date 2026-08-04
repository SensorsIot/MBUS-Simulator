// Host-tier tests — FSD §12.
//
// Every case is registered separately in CMakeLists.txt under the requirement it
// discharges, so a red build names the property that broke.
//
// The oracle in `decode` is dlms_parser v2.1.0 — the decoder gplug-mini actually
// runs. That is the whole design: if the DUT's decoder cannot read our bytes on
// a laptop, it will not read them on the wire either, and we find out in seconds
// instead of on the bench. Note what this proves and what it does not: it proves
// the output is *decodable*, not that it is *faithful*. A tolerant decoder would
// accept telegrams a real E450 never emits.

#include <dlms_parser/dlms_parser.h>
#include <e450/ebl_ci1.h>
#include <e450/telegram.h>
#include <e450/value_model.h>

#include <cinttypes>
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

// Run the bytes through the DUT's decoder and collect every numeric capture,
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

// FR-ENC-01..04: the DUT's decoder reads our telegram and recovers the model.
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

// FR-ENC-05: the invoke id counts telegrams. A DUT that dedupes on it would
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

// FR-SCH-08 — the telegram must leave the reader a silent gap long enough to
// call the transmission finished.
//
// This is a host test because the arithmetic is fully determined here: the CI1
// list is fixed and every value is a fixed-width encoding, so the telegram is
// the same size whatever the model holds. If it ever stops fitting — a longer
// object list, a second push setup, a wider value — the failure is a merged
// burst at the far end, which presents as a decode fault in the *reader* and
// gets debugged there. Catching it in ctest costs seconds instead.
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
    check(silence >= e450::DUT_BURST_TIMEOUT_MS, "5 s period clears the reader's 2 s timeout");

    // The rounding must be conservative, or a telegram that just misses passes.
    check(e450::wire_time_ms(1) == 5, "one byte is 11 bits = 4.58 ms, rounded up");
    check(e450::wire_time_ms(0) == 0, "no bytes, no time");
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
    else {
        std::fprintf(stderr, "unknown case: %s\n", name.c_str());
        return 2;
    }

    std::printf("%s: %d failure(s)\n", name.c_str(), failures);
    return failures == 0 ? 0 : 1;
}

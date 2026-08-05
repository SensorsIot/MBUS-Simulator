// The ESP32 shell — FSD §2.2 L0.
//
// Everything protocol-shaped lives in components/e450_core, which knows nothing
// about ESP-IDF and is therefore testable on a laptop. What is left here is the
// part that genuinely needs hardware: a UART, a GPIO, a clock and a console.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "e450/datetime.h"
#include "e450/directives.h"
#include "e450/ebl_ci1.h"
#include "e450/emission.h"
#include "e450/faults.h"
#include "e450/modes.h"
#include "e450/rng.h"
#include "e450/telegram.h"
#include "e450/value_model.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

const char *TAG = "sim";

// A Kconfig bool set to 'n' is *undefined*, not 0, so it cannot be used in an
// expression — only in #if. Normalising it here once means the rest of the file
// can treat inversion as an ordinary boolean, and flipping it to 'n' to test the
// polarity of a driver stage does not break the build.
#if CONFIG_SIM_TX_INVERT
constexpr bool TX_INVERTED = true;
#else
constexpr bool TX_INVERTED = false;
#endif

constexpr uart_port_t METER_UART = UART_NUM_2;
constexpr auto TX_GPIO = static_cast<gpio_num_t>(CONFIG_SIM_MBUS_TX_GPIO);

// FR-SCH-01: emit when the clock's seconds field is divisible by the period.
// SIM-3 / FR-FLT-17. Optional feedback from the driver stage's *output* back
// into a GPIO. Without it the firmware cannot see the line at all — it only
// knows when it handed bytes to the UART — so `lead_us` reports n/a rather than
// a number that would look like a measurement and be a guess.
constexpr int LINE_SENSE_GPIO = CONFIG_SIM_LINE_SENSE_GPIO;

constexpr int PUSH_PERIOD_SECONDS = CONFIG_SIM_PUSH_PERIOD_SECONDS;
static_assert(60 % PUSH_PERIOD_SECONDS == 0,
              "SIM_PUSH_PERIOD_SECONDS must divide 60, or emission is uneven "
              "across the minute boundary (FR-SCH-01)");

// How often the scheduler task wakes to re-read the wall clock. Fine enough that
// a 5 s boundary is never missed, coarse enough to cost nothing.
constexpr TickType_t SCHEDULER_POLL_MS = 50;

e450::ValueModel g_model;
e450::SimClock g_clock{e450::DateTime{}};
SemaphoreHandle_t g_lock = nullptr;
// A second mutex, guarding the UART rather than the model. Without it the `tx`
// bring-up pattern and a scheduled telegram could interleave on the line — the
// one thing FR-SCH-06 forbids.
SemaphoreHandle_t g_tx_lock = nullptr;
// FR-MOD-01. The one control the operator has: which of the three things this
// simulator can put on the wire it is currently putting there. Mode 3 at boot,
// because the faithful behaviour is the default and the other two are what you
// drop to when it does not work.
e450::Mode g_mode = e450::Mode::E450;

// FR-MOD-04. Seeded once at boot and never reseeded, so a run from reset
// replays byte for byte (FR-CLK-04) while still being unpredictable downstream.
e450::Rng g_rng{e450::MODE3_SEED};

uint32_t g_invoke_id = 0;
uint32_t g_emissions = 0;

// FSD §14. Persistent directives, and the one-shot fault armed for the next
// emission. The fault is deliberately not part of EmissionConfig: one is state
// the operator sets and leaves, the other clears itself, and conflating them is
// how a corruption silently outlives the case it was armed for.
e450::EmissionConfig g_cfg;
e450::FaultSpec g_fault;
int64_t g_silence_until_us = 0;

// Written from an ISR, read from the scheduler task. volatile is enough: a
// 64-bit read is not atomic on Xtensa, but the ISR only writes while armed and
// the reader only looks after disarming, so they never overlap.
volatile int64_t g_sense_first_edge_us = 0;
volatile bool g_sense_armed = false;

bool line_sense_available() { return LINE_SENSE_GPIO >= 0; }

struct Lock {
    Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_lock); }
};

// ------------------------------------------------------------------ hardware

void IRAM_ATTR line_sense_isr(void *) {
    // First edge only. Everything after it is data, and timing those would
    // measure the baud rate we already know.
    if (g_sense_armed) {
        g_sense_first_edge_us = esp_timer_get_time();
        g_sense_armed = false;
    }
}

void configure_line_sense() {
    if (!line_sense_available()) {
        ESP_LOGI(TAG, "no line-sense GPIO: lead_us will report n/a (FR-FLT-17)");
        return;
    }
    const auto pin = static_cast<gpio_num_t>(LINE_SENSE_GPIO);
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(pin, &line_sense_isr, nullptr));
    ESP_LOGI(TAG, "line sense on GPIO%d", LINE_SENSE_GPIO);
}

void configure_meter_link() {
    // FR-LNK-05, and the one that bites if skipped. The idle level that puts
    // 24 V — the mark state — on the bus is HIGH for this rig's non-inverting
    // driver, and LOW if TX inversion is turned back on for an inverting one;
    // either way it is the UART's idle level, computed here from the same
    // constant. Between reset and uart_set_pin() the pin is a floating input, so
    // without driving it here the bus level is undefined on every reboot and the
    // bus carries a break condition or noise instead of resting quiet.
    gpio_config_t idle = {};
    idle.pin_bit_mask = 1ULL << TX_GPIO;
    idle.mode = GPIO_MODE_OUTPUT;
    idle.pull_up_en = GPIO_PULLUP_DISABLE;
    idle.pull_down_en = GPIO_PULLDOWN_DISABLE;
    idle.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&idle));
    gpio_set_level(TX_GPIO, TX_INVERTED ? 0 : 1);

    uart_config_t cfg = {};
    cfg.baud_rate = 2400;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_EVEN;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    // A TX ring buffer, and no RX buffer at all: the simulator never receives on
    // the meter link, and the real meter never listens either (FR-LNK-03).
    // The RX size is a floor the driver insists on (it must exceed the hardware
    // FIFO), not a buffer this firmware has any use for.
    ESP_ERROR_CHECK(uart_driver_install(METER_UART, 256, 1024, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(METER_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(METER_UART, TX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    if (TX_INVERTED) {
        ESP_ERROR_CHECK(uart_set_line_inverse(METER_UART, UART_SIGNAL_TXD_INV));
    }
}

// ----------------------------------------------------------------- scheduler

const char *mode_name(e450::Mode mode) {
    switch (mode) {
        case e450::Mode::Pattern: return "pattern";
        case e450::Mode::Simple: return "simple";
        case e450::Mode::E450: return "e450";
        case e450::Mode::Ramp: return "ramp";
    }
    return "?";
}

// What this mode puts on the line, plus a description of what is in it.
//
// The description is not decoration. In mode 3 the values change every emission,
// so the only way to check a decode is to compare it against what was actually
// sent — and this line is that record.
struct Payload {
    std::vector<uint8_t> wire;
    uint32_t id = 0;
    std::size_t frames = 0;
    char fault[40] = "none";
    char note[96] = {};
};

Payload build_payload(e450::Mode mode, const e450::EmissionConfig &cfg,
                      const e450::FaultSpec &fault) {
    Payload out;
    e450::fault_to_string(fault, out.fault, sizeof(out.fault));

    // FR-FLT-07. Noise replaces the telegram outright — there is nothing to
    // corrupt because there is nothing valid in it to begin with.
    if (fault.kind == e450::Fault::Noise) {
        out.wire = e450::noise_bytes(fault.arg);
        Lock held;
        out.id = g_emissions++;
        std::snprintf(out.note, sizeof(out.note), "garbage=%u", unsigned(fault.arg));
        return out;
    }

    if (mode == e450::Mode::Ramp && !fault.armed()) {
        // FR-MOD-06. The length is taken from an actual mode 3 encoding rather
        // than from a constant, so "the same length and duration as a telegram"
        // stays true when a directive changes the telegram's size.
        std::size_t length = 0;
        {
            Lock held;
            const e450::Emission shape = e450::plan_e450(g_model, cfg, e450::FaultSpec{});
            for (const std::vector<uint8_t> &f :
                 e450::encode_telegram_frames(shape.spec(0, g_clock.now()))) {
                length += f.size();
            }
            length += cfg.preamble_bytes;
            out.id = g_emissions++;
        }
        out.wire = e450::ramp_bytes(length);
        std::snprintf(out.note, sizeof(out.note), "ramp=00..FF first=0x00");
        return out;
    }

    if (mode == e450::Mode::Pattern && !fault.armed()) {
        // FR-MOD-02. No protocol at all: 0x55 alternates every bit, so what
        // comes out at the far end reports the line settings and nothing else.
        out.wire.assign(e450::PATTERN_BURST_BYTES, e450::PATTERN_BYTE);
        Lock held;
        out.id = g_emissions++;
        std::snprintf(out.note, sizeof(out.note), "pattern=0x%02X", e450::PATTERN_BYTE);
        return out;
    }

    // `plan` owns the objects and values; the spec only views them, so it must
    // not outlive this scope (TelegramSpec::bind_values).
    e450::Emission plan;
    uint32_t invoke_id = 0;
    e450::DateTime when{};
    {
        Lock held;
        invoke_id = g_invoke_id++;
        when = g_clock.now();
        out.id = g_emissions++;

        if (fault.armed()) {
            // FR-FLT-01. A fault always rides on the fixed load, whatever the
            // mode: an arithmetic assertion cannot be made against a value that
            // changes every emission, and a fault case exists to be asserted on.
            const e450::ValueModel fixed = e450::fixed_model();
            plan = e450::plan_e450(fixed, cfg, fault);
            std::snprintf(out.note, sizeof(out.note), "pi=%" PRIu32 " ei=%" PRIu32 " fixed=1",
                          fixed.import_w(), cfg.energy_set ? cfg.energy_wh : fixed.import_wh());
        } else if (mode == e450::Mode::Simple) {
            plan = e450::plan_simple(cfg, fault);
            std::snprintf(out.note, sizeof(out.note), "p=%" PRIu32 " e=%" PRIu32,
                          e450::simple::ACTIVE_POWER_W,
                          cfg.energy_set ? cfg.energy_wh : e450::simple::ACTIVE_ENERGY_WH);
        } else {
            plan = e450::plan_e450(g_model, cfg, fault);
            std::snprintf(out.note, sizeof(out.note),
                          "pi=%" PRIu32 " po=%" PRIu32 " ei=%" PRIu32 " eo=%" PRIu32
                          " i=%" PRIu32,
                          g_model.import_w(), g_model.export_w(),
                          cfg.energy_set ? cfg.energy_wh : g_model.import_wh(),
                          g_model.export_wh(), g_model.current_ma());
        }
    }

    const std::vector<std::vector<uint8_t>> frames =
        e450::encode_telegram_frames(plan.spec(invoke_id, when));
    out.wire = e450::apply_wire_fault(frames, fault, cfg.preamble_bytes, cfg.preamble_byte);
    out.frames = frames.size();
    if (fault.kind == e450::Fault::DropBlock && fault.arg <= out.frames && out.frames > 0) {
        --out.frames;
    }
    return out;
}

void emit(e450::Mode mode, const e450::EmissionConfig &cfg, const e450::FaultSpec &fault) {
    const Payload payload = build_payload(mode, cfg, fault);

    // Blocking, deliberately. At 2400 baud a telegram takes about 1.7 s, and the
    // next tick is 5 s away — but more importantly this is what makes FR-SCH-06
    // true by construction: one task owns the line, so two emissions can never
    // interleave on it. The clock is driven from esp_timer deltas, not from this
    // task's progress, so blocking here costs no time.
    xSemaphoreTake(g_tx_lock, portMAX_DELAY);
    // SIM-3 / FR-FLT-17. Measured, not computed: `wire_ms` is arithmetic on the
    // byte count and would report 1747 ms even if the UART were misconfigured.
    // `tx_ms` is what actually happened, so the two disagreeing is a finding.
    const int64_t tx_begin_us = esp_timer_get_time();
    g_sense_first_edge_us = 0;
    g_sense_armed = true;
    uart_write_bytes(METER_UART, payload.wire.data(), payload.wire.size());
    uart_wait_tx_done(METER_UART, portMAX_DELAY);
    const int64_t tx_end_us = esp_timer_get_time();
    g_sense_armed = false;
    xSemaphoreGive(g_tx_lock);

    const int64_t edge_us = g_sense_first_edge_us;
    char lead[16];
    if (!line_sense_available()) {
        std::snprintf(lead, sizeof(lead), "n/a");
    } else if (edge_us == 0) {
        std::snprintf(lead, sizeof(lead), "none");
    } else {
        std::snprintf(lead, sizeof(lead), "%lld", static_cast<long long>(edge_us - tx_begin_us));
    }

    // Logged on the console so "is it emitting?" is answerable without a scope.
    // That is the first cut of any no-signal investigation: this line appearing
    // every period puts the fault after the pin, its absence puts it before.
    //
    // The silence figure is here rather than only at boot because it is the one
    // number that decides whether the line carries two emissions or one merged
    // burst (FR-SCH-08), and it moves whenever the payload does.
    // FR-FLT-14: strictly key=value, so a bench script can assert on what was
    // emitted rather than infer it. "Frame 2 of 3 was corrupted" has to be a
    // statement the simulator makes, or the only available observation is that
    // something downstream discarded something.
    const uint32_t period_ms = cfg.gap_ms > 0
                                   ? e450::wire_time_ms(payload.wire.size()) + cfg.gap_ms
                                   : static_cast<uint32_t>(PUSH_PERIOD_SECONDS) * 1000u;
    const int32_t silence = e450::silence_ms(payload.wire.size(), period_ms);
    ESP_LOGI(TAG,
             "emit n=%" PRIu32 " mode=%s fault=%s bytes=%u frames=%u pre=%" PRIu32
             "/0x%02X wire_ms=%" PRIu32 " tx_ms=%lld lead_us=%s silent_ms=%" PRId32 " gpio=%d %s",
             payload.id, mode_name(mode), payload.fault,
             static_cast<unsigned>(payload.wire.size()), static_cast<unsigned>(payload.frames),
             cfg.preamble_bytes, unsigned(cfg.preamble_byte),
             e450::wire_time_ms(payload.wire.size()),
             static_cast<long long>((tx_end_us - tx_begin_us) / 1000), lead, silence,
             CONFIG_SIM_MBUS_TX_GPIO, payload.note);

    // FR-SCH-08. Loud, and only once: a periodic error line would bury the log
    // it is trying to qualify, but a silent one lets the rig go on producing
    // bursts that merge — which looks like a decode fault at the far end and
    // gets debugged there, wrongly.
    static bool complained = false;
    if (silence < e450::MIN_SILENCE_MS && cfg.gap_ms == 0 && !complained) {
        complained = true;
        ESP_LOGE(TAG,
                 "only %" PRId32 " ms of silence between emissions, want >= %" PRId32 " ms: "
                 "a transmission ends after %" PRId32 " ms of quiet, so consecutive "
                 "emissions may be read as one. Raise SIM_PUSH_PERIOD_SECONDS.",
                 silence, e450::MIN_SILENCE_MS, e450::BURST_TIMEOUT_MS);
    }
}

[[noreturn]] void scheduler_task(void *) {
    int64_t last_us = esp_timer_get_time();
    int last_emitted_second = -1;
    int64_t next_free_run_us = 0;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const auto elapsed_ms = static_cast<uint32_t>((now_us - last_us) / 1000);
        if (elapsed_ms > 0) {
            last_us += static_cast<int64_t>(elapsed_ms) * 1000;
            Lock held;
            g_clock.advance_ms(elapsed_ms);
        }

        bool due = false;
        e450::Mode mode = e450::Mode::E450;
        e450::EmissionConfig cfg;
        e450::FaultSpec fault;
        {
            Lock held;
            const e450::DateTime now = g_clock.now();
            mode = g_mode;
            cfg = g_cfg;
            fault = g_fault;

            if (now_us < g_silence_until_us) {
                // FR-FLT-09. Silence is a capability, not the absence of one:
                // the clock keeps running, the console keeps answering, and
                // nothing reaches the line. Consume the boundary so that the
                // first telegram after the silence lands on the schedule rather
                // than immediately.
                if (now.second % PUSH_PERIOD_SECONDS == 0) last_emitted_second = now.second;
            } else if (cfg.gap_ms > 0) {
                // FR-FLT-08. Free-running: the gap is measured from the last
                // byte out to the first byte of the next telegram, which is the
                // only way to place a boundary at 1999 vs 2000 ms. Wall-clock
                // alignment (FR-SCH-01) is deliberately suspended while set.
                due = (now_us >= next_free_run_us);
            } else if ((now.second % PUSH_PERIOD_SECONDS == 0) &&
                       (now.second != last_emitted_second)) {
                last_emitted_second = now.second;
                due = true;
            }

            if (due && mode == e450::Mode::E450 && !fault.armed()) {
                // FR-MOD-04, and the order matters. Choosing the load before
                // integrating means the energy this telegram reports is exactly
                // the power this telegram reports, multiplied by the period — so
                // the arithmetic can be checked against a single message, with
                // no reference to what came before.
                e450::apply_random_load(g_model, g_rng);
                g_model.tick(PUSH_PERIOD_SECONDS * 1000);  // FR-SCH-03
            }
        }

        if (due) {
            emit(mode, cfg, fault);
            Lock held;
            // FR-FLT-01: one fault, one telegram. A sticky corruption poisons
            // every later case in a run, so it clears itself here rather than
            // waiting to be cleared by an operator who may forget.
            g_fault = e450::FaultSpec{};
            if (cfg.gap_ms > 0) {
                next_free_run_us = esp_timer_get_time() + static_cast<int64_t>(cfg.gap_ms) * 1000;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SCHEDULER_POLL_MS));
    }
}

// ------------------------------------------------------------------- console

// FR-CLK-02. The clock is the only thing left to set: the load is no longer an
// operator input at all, because each mode decides its own values (fixed in
// mode 2, chosen by the seeded generator in mode 3). Setting the clock stays,
// because a repeatable run needs a repeatable starting point.
int cmd_set(int argc, char **argv) {
    if (argc < 3 || std::strcmp(argv[1], "time") != 0) {
        std::printf("ERR usage: set time <YYYY-MM-DDTHH:MM:SS>\n");
        return 1;
    }

    e450::DateTime dt;
    unsigned year, month, day, hour, minute, second;
    if (std::sscanf(argv[2], "%4u-%2u-%2uT%2u:%2u:%2u", &year, &month, &day, &hour, &minute,
                    &second) != 6 ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 59) {
        std::printf("ERR expected YYYY-MM-DDTHH:MM:SS\n");
        return 1;
    }
    dt.year = static_cast<uint16_t>(year);
    dt.month = static_cast<uint8_t>(month);
    dt.day = static_cast<uint8_t>(day);
    dt.hour = static_cast<uint8_t>(hour);
    dt.minute = static_cast<uint8_t>(minute);
    dt.second = static_cast<uint8_t>(second);

    Lock held;
    g_clock.set(dt);
    std::printf("OK\n");
    return 0;
}

// FR-MOD-01. With no argument it reports the current mode, so a script can read
// the state it is about to change.
int cmd_mode(int argc, char **argv) {
    if (argc < 2) {
        Lock held;
        std::printf("OK %d %s\n", static_cast<int>(g_mode), mode_name(g_mode));
        return 0;
    }

    char *end = nullptr;
    const long which = std::strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != 0 || which < 1 || which > 4) {
        std::printf("ERR usage: mode [1|2|3|4]  (1 pattern, 2 simple, 3 e450, 4 ramp)\n");
        return 1;
    }

    const auto mode = static_cast<e450::Mode>(which);
    {
        Lock held;
        g_mode = mode;
    }
    // Takes effect at the next period boundary, not immediately: an emission
    // already on the wire is left to finish, because truncating it would hand
    // out a malformed frame and a fault that is not real.
    std::printf("OK %ld %s\n", which, mode_name(mode));
    return 0;
}

// FR-CLI-04: one machine-parsable line, so a test script can assert on it
// without a parser.
const char *identity_name(e450::Identity id) {
    switch (id) {
        case e450::Identity::Ldn: return "ldn";
        case e450::Identity::DevId: return "devid";
        case e450::Identity::None: return "none";
    }
    return "?";
}

int cmd_status(int, char **) {
    Lock held;
    const e450::DateTime now = g_clock.now();
    const int64_t left_us = g_silence_until_us - esp_timer_get_time();
    const int silence_left = left_us > 0 ? static_cast<int>(left_us / 1000000) : 0;
    char fault_text[40];
    e450::fault_to_string(g_fault, fault_text, sizeof(fault_text));
    char energy_text[16];
    if (g_cfg.energy_set) {
        std::snprintf(energy_text, sizeof(energy_text), "%" PRIu32, g_cfg.energy_wh);
    } else {
        std::snprintf(energy_text, sizeof(energy_text), "auto");
    }
    // PRIu32 rather than %u: uint32_t is `long unsigned int` on Xtensa, and
    // -Werror=format catches the mismatch that would be silent on the host.
    std::printf(
        "OK pi=%" PRIu32 " po=%" PRIu32 " pf=%.2f%s ei=%" PRIu32 " eo=%" PRIu32 " q1=%" PRIu32
        " q2=%" PRIu32 " q3=%" PRIu32 " q4=%" PRIu32 " i=%" PRIu32
        " time=%04u-%02u-%02uT%02u:%02u:%02u mode=%d/%s period=%d gap=%" PRIu32
        " silence=%d preamble=%" PRIu32 "/0x%02X serial=%u identity=%s scaler=%d energy=%s fault=%s"
        " emissions=%" PRIu32 "\n",
        g_model.import_w(), g_model.export_w(), static_cast<double>(g_model.power_factor()),
        g_model.lagging() ? "" : "L", g_model.import_wh(), g_model.export_wh(),
        g_model.reactive_varh(e450::ValueModel::Q1_IMPORT_LAGGING),
        g_model.reactive_varh(e450::ValueModel::Q2_EXPORT_LAGGING),
        g_model.reactive_varh(e450::ValueModel::Q3_EXPORT_LEADING),
        g_model.reactive_varh(e450::ValueModel::Q4_IMPORT_LEADING), g_model.current_ma(), now.year,
        now.month, now.day, now.hour, now.minute, now.second, static_cast<int>(g_mode),
        mode_name(g_mode), PUSH_PERIOD_SECONDS, g_cfg.gap_ms, silence_left,
        g_cfg.preamble_bytes, unsigned(g_cfg.preamble_byte), g_cfg.serial_chars,
        identity_name(g_cfg.identity), g_cfg.scaler, energy_text, fault_text, g_emissions);
    return 0;
}

// FSD §14 — the runtime directives. Every one of them is here rather than in
// Kconfig on purpose: a fault that needs a rebuild makes the firmware image part
// of the test fixture, and a bench run then cannot say which simulator produced
// its evidence.

bool parse_long(const char *text, long lo, long hi, long *out) {
    char *end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != 0 || v < lo || v > hi) return false;
    *out = v;
    return true;
}

// "1.1.1.8.0.255" — the spelling a decoder prints, so an operator can paste back
// what they just read rather than translating it.
bool parse_obis(const char *text, std::array<uint8_t, 6> *out) {
    unsigned a, b, c, d, e, f;
    if (std::sscanf(text, "%u.%u.%u.%u.%u.%u", &a, &b, &c, &d, &e, &f) != 6) return false;
    const unsigned parts[6] = {a, b, c, d, e, f};
    for (unsigned v : parts) {
        if (v > 255) return false;
    }
    for (int i = 0; i < 6; ++i) (*out)[i] = static_cast<uint8_t>(parts[i]);
    return true;
}

int cmd_fault(int argc, char **argv) {
    char text[40];
    if (argc < 2) {
        Lock held;
        std::printf("OK %s\n", e450::fault_to_string(g_fault, text, sizeof(text)));
        return 0;
    }

    e450::FaultSpec f;
    const char *what = argv[1];
    long n = 0;

    if (std::strcmp(what, "none") == 0) {
        // f stays cleared
    } else if (std::strcmp(what, "start-mid-frame") == 0) {
        f.kind = e450::Fault::StartMidFrame;
    } else if (std::strcmp(what, "duplicate-register") == 0) {
        if (argc < 3 || !parse_obis(argv[2], &f.obis)) {
            std::printf("ERR usage: fault duplicate-register <a.b.c.d.e.f>\n");
            return 1;
        }
        f.kind = e450::Fault::DuplicateRegister;
    } else if (std::strcmp(what, "fcs") == 0 || std::strcmp(what, "truncate") == 0 ||
               std::strcmp(what, "drop-block") == 0) {
        if (argc < 3 || !parse_long(argv[2], 1, 16, &n)) {
            std::printf("ERR usage: fault %s <1..16>\n", what);
            return 1;
        }
        f.kind = std::strcmp(what, "fcs") == 0          ? e450::Fault::Fcs
                 : std::strcmp(what, "truncate") == 0   ? e450::Fault::Truncate
                                                        : e450::Fault::DropBlock;
        f.arg = static_cast<uint32_t>(n);
    } else if (std::strcmp(what, "noise") == 0) {
        if (argc < 3 || !parse_long(argv[2], 1, 2048, &n)) {
            std::printf("ERR usage: fault noise <1..2048>\n");
            return 1;
        }
        f.kind = e450::Fault::Noise;
        f.arg = static_cast<uint32_t>(n);
    } else {
        std::printf("ERR unknown fault '%s'\n", what);
        return 1;
    }

    {
        Lock held;
        g_fault = f;
    }
    std::printf("OK %s armed for the next telegram\n",
                e450::fault_to_string(f, text, sizeof(text)));
    return 0;
}

int cmd_preamble(int argc, char **argv) {
    long n = 0;
    if (argc < 2 || !parse_long(argv[1], 0, 512, &n)) {
        std::printf("ERR usage: preamble <0..512> [filler-byte, default 0x7E]\n");
        return 1;
    }
    long filler = e450::PREAMBLE_BYTE;
    if (argc >= 3) {
        char *end = nullptr;
        filler = std::strtol(argv[2], &end, 0);  // 0 => accepts 0x7E and 126 alike
        if (end == argv[2] || *end != 0 || filler < 0 || filler > 255) {
            std::printf("ERR filler must be 0..255 (0x00..0xFF)\n");
            return 1;
        }
    }
    Lock held;
    g_cfg.preamble_bytes = static_cast<uint32_t>(n);
    g_cfg.preamble_byte = static_cast<uint8_t>(filler);
    std::printf("OK preamble=%ld bytes of 0x%02X\n", n, unsigned(filler));
    return 0;
}

int cmd_gap(int argc, char **argv) {
    long ms = 0;
    if (argc < 2 || !parse_long(argv[1], 0, 60000, &ms)) {
        std::printf("ERR usage: gap <0..60000 ms>  (0 restores the %d s schedule)\n",
                    PUSH_PERIOD_SECONDS);
        return 1;
    }
    Lock held;
    g_cfg.gap_ms = static_cast<uint32_t>(ms);
    std::printf("OK gap=%ld%s\n", ms, ms == 0 ? " (clock-aligned)" : " (free-running)");
    return 0;
}

int cmd_silence(int argc, char **argv) {
    long seconds = 0;
    if (argc < 2 || !parse_long(argv[1], 0, 3600, &seconds)) {
        std::printf("ERR usage: silence <0..3600 seconds>\n");
        return 1;
    }
    Lock held;
    g_silence_until_us = esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000;
    std::printf("OK silent for %ld s\n", seconds);
    return 0;
}

int cmd_serial(int argc, char **argv) {
    long chars = 0;
    if (argc < 2 || !parse_long(argv[1], 8, 16, &chars) || (chars != 8 && chars != 16)) {
        std::printf("ERR usage: serial <8|16>\n");
        return 1;
    }
    Lock held;
    g_cfg.serial_chars = static_cast<uint8_t>(chars);
    std::printf("OK serial=%ld\n", chars);
    return 0;
}

int cmd_identity(int argc, char **argv) {
    if (argc < 2) {
        std::printf("ERR usage: identity <ldn|devid|none>\n");
        return 1;
    }
    e450::Identity id;
    if (std::strcmp(argv[1], "ldn") == 0) {
        id = e450::Identity::Ldn;
    } else if (std::strcmp(argv[1], "devid") == 0) {
        id = e450::Identity::DevId;
    } else if (std::strcmp(argv[1], "none") == 0) {
        id = e450::Identity::None;
    } else {
        std::printf("ERR usage: identity <ldn|devid|none>\n");
        return 1;
    }
    Lock held;
    g_cfg.identity = id;
    std::printf("OK identity=%s\n", argv[1]);
    return 0;
}

int cmd_energy(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "auto") == 0) {
        Lock held;
        g_cfg.energy_set = false;
        std::printf("OK energy=auto\n");
        return 0;
    }
    char *end = nullptr;
    const unsigned long wh = (argc >= 2) ? std::strtoul(argv[1], &end, 10) : 0;
    if (argc < 2 || end == argv[1] || *end != 0 || wh > 4294967295UL) {
        std::printf("ERR usage: energy <wh> | auto\n");
        return 1;
    }
    Lock held;
    g_cfg.energy_set = true;
    g_cfg.energy_wh = static_cast<uint32_t>(wh);
    std::printf("OK energy=%lu\n", wh);
    return 0;
}

int cmd_scaler(int argc, char **argv) {
    long n = 0;
    if (argc < 2 || !parse_long(argv[1], -9, 9, &n)) {
        std::printf("ERR usage: scaler <-9..9>  (0 restores bare values)\n");
        return 1;
    }
    Lock held;
    g_cfg.scaler = static_cast<int8_t>(n);
    std::printf("OK scaler=%ld\n", n);
    return 0;
}

struct CommandDef {
    const char *command;
    const char *help;
    esp_console_cmd_func_t func;
};

void register_commands() {
    static constexpr CommandDef DEFS[] = {
        {"mode", "mode [1|2|3|4] - 1 pattern, 2 simple, 3 full E450, 4 counted ramp",
         &cmd_mode},
        {"status", "print the whole state as one key=value line", &cmd_status},
        {"set", "set time <YYYY-MM-DDTHH:MM:SS>", &cmd_set},
        {"fault",
         "fault [none|fcs N|truncate N|drop-block N|start-mid-frame|"
         "duplicate-register OBIS|noise N] - one telegram, then clears",
         &cmd_fault},
        {"preamble", "preamble <n> [byte] - n bytes of lead-in before the first frame",
         &cmd_preamble},
        {"gap", "gap <ms> - silence between telegrams; 0 restores the schedule", &cmd_gap},
        {"silence", "silence <seconds> - emit nothing, keep running", &cmd_silence},
        {"serial", "serial <8|16> - length of the identity serial", &cmd_serial},
        {"identity", "identity <ldn|devid|none> - which identity object appears", &cmd_identity},
        {"energy", "energy <wh>|auto - set the cumulative import register", &cmd_energy},
        {"scaler", "scaler <-9..9> - emit values with a scaler; 0 for bare", &cmd_scaler},
    };

    // Zero-initialise and assign rather than brace-initialise: esp_console_cmd_t
    // has grown fields (func_w_context, context) and would grow again, and
    // -Werror=missing-field-initializers turns every such addition into a build
    // break for every caller that spelled the struct out positionally.
    for (const CommandDef &def : DEFS) {
        esp_console_cmd_t cmd = {};
        cmd.command = def.command;
        cmd.help = def.help;
        cmd.func = def.func;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
}

e450::DateTime parse_epoch() {
    e450::DateTime dt;
    unsigned year, month, day, hour, minute, second;
    if (std::sscanf(CONFIG_SIM_EPOCH, "%4u-%2u-%2uT%2u:%2u:%2u", &year, &month, &day, &hour,
                    &minute, &second) == 6) {
        dt.year = static_cast<uint16_t>(year);
        dt.month = static_cast<uint8_t>(month);
        dt.day = static_cast<uint8_t>(day);
        dt.hour = static_cast<uint8_t>(hour);
        dt.minute = static_cast<uint8_t>(minute);
        dt.second = static_cast<uint8_t>(second);
    } else {
        ESP_LOGW(TAG, "CONFIG_SIM_EPOCH is malformed; falling back to the struct default");
    }
    return dt;
}

}  // namespace

extern "C" void app_main() {
    g_lock = xSemaphoreCreateMutex();
    g_tx_lock = xSemaphoreCreateMutex();
    g_clock.set(parse_epoch());

    configure_meter_link();
    configure_line_sense();

    ESP_LOGI(TAG, "E450 simulator: mode %d (%s), every %d s, 2400 8E1%s",
             static_cast<int>(g_mode), mode_name(g_mode), PUSH_PERIOD_SECONDS,
             TX_INVERTED ? " inverted" : "");
    ESP_LOGI(TAG, "modes: 1 pattern (is there a signal?), 2 simple (can it decode?), "
                  "3 e450 (can it decode the real thing?), 4 ramp (how many bytes were lost?)");

    xTaskCreate(&scheduler_task, "scheduler", 6144, nullptr, 5, nullptr);

    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "sim>";
    repl_config.max_cmdline_length = 128;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    esp_console_register_help_command();
    register_commands();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

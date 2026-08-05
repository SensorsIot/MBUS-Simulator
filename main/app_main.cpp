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
#include "e450/ebl_ci1.h"
#include "e450/modes.h"
#include "e450/rng.h"
#include "e450/telegram.h"
#include "e450/value_model.h"
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
// replays byte for byte (FR-CLK-04) while still being unpredictable to a reader.
e450::Rng g_rng{e450::MODE3_SEED};

uint32_t g_invoke_id = 0;
uint32_t g_emissions = 0;

struct Lock {
    Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_lock); }
};

// ------------------------------------------------------------------ hardware

void configure_meter_link() {
    // FR-LNK-05, and the one that bites if skipped. The idle level that puts
    // 24 V — the mark state — on the bus is HIGH for this rig's non-inverting
    // driver, and LOW if TX inversion is turned back on for an inverting one;
    // either way it is the UART's idle level, computed here from the same
    // constant. Between reset and uart_set_pin() the pin is a floating input, so
    // without driving it here the bus level is undefined on every reboot and the
    // reader sees a break condition or noise instead of a quiet line.
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
    }
    return "?";
}

// What this mode puts on the line, plus a description of what is in it.
//
// The description is not decoration. In mode 3 the values change every emission,
// so the only way to check a reader is to compare what it reported against what
// was actually sent — and this line is that record.
struct Payload {
    std::vector<uint8_t> wire;
    uint32_t id = 0;
    char note[96] = {};
};

Payload build_payload(e450::Mode mode) {
    Payload out;

    if (mode == e450::Mode::Pattern) {
        // FR-MOD-02. No protocol at all: 0x55 alternates every bit, so what
        // comes out at the far end reports the line settings and nothing else.
        out.wire.assign(e450::PATTERN_BURST_BYTES, e450::PATTERN_BYTE);
        Lock held;
        out.id = g_emissions++;
        std::snprintf(out.note, sizeof(out.note), "0x%02X x %u", e450::PATTERN_BYTE,
                      static_cast<unsigned>(e450::PATTERN_BURST_BYTES));
        return out;
    }

    // `values` must outlive the encode call below — TelegramSpec holds a view of
    // it, not a copy.
    std::vector<e450::TelegramValue> values;
    e450::TelegramSpec spec;
    {
        Lock held;
        spec.invoke_id = g_invoke_id++;
        spec.timestamp = g_clock.now();
        out.id = g_emissions++;

        if (mode == e450::Mode::Simple) {
            spec.objects = e450::simple::objects();
            values = e450::simple::values();
            std::snprintf(out.note, sizeof(out.note), "p=%" PRIu32 " W e=%" PRIu32 " Wh",
                          e450::simple::ACTIVE_POWER_W, e450::simple::ACTIVE_ENERGY_WH);
        } else {
            spec.objects = e450::ebl::ci1_objects();
            values = e450::ebl::ci1_values(g_model);
            std::snprintf(out.note, sizeof(out.note),
                          "pi=%" PRIu32 " po=%" PRIu32 " ei=%" PRIu32 " eo=%" PRIu32
                          " i=%" PRIu32,
                          g_model.import_w(), g_model.export_w(), g_model.import_wh(),
                          g_model.export_wh(), g_model.current_ma());
        }
    }

    spec.bind_values(values);
    out.wire = e450::encode_telegram(spec);
    return out;
}

void emit(e450::Mode mode) {
    const Payload payload = build_payload(mode);

    // Blocking, deliberately. At 2400 baud a telegram takes about 1.7 s, and the
    // next tick is 5 s away — but more importantly this is what makes FR-SCH-06
    // true by construction: one task owns the line, so two emissions can never
    // interleave on it. The clock is driven from esp_timer deltas, not from this
    // task's progress, so blocking here costs no time.
    xSemaphoreTake(g_tx_lock, portMAX_DELAY);
    uart_write_bytes(METER_UART, payload.wire.data(), payload.wire.size());
    uart_wait_tx_done(METER_UART, portMAX_DELAY);
    xSemaphoreGive(g_tx_lock);

    // Logged on the console so "is it emitting?" is answerable without a scope.
    // That is the first cut of any no-signal investigation: this line appearing
    // every period puts the fault after the pin, its absence puts it before.
    //
    // The silence figure is here rather than only at boot because it is the one
    // number that decides whether the reader sees two emissions or one merged
    // burst (FR-SCH-08), and it moves whenever the payload does.
    const int32_t silence = e450::silence_ms(payload.wire.size(), PUSH_PERIOD_SECONDS * 1000);
    ESP_LOGI(TAG,
             "%s %" PRIu32 ": %u bytes on GPIO%d, %" PRIu32 " ms on the wire, %" PRId32
             " ms silent - %s",
             mode_name(mode), payload.id, static_cast<unsigned>(payload.wire.size()),
             CONFIG_SIM_MBUS_TX_GPIO, e450::wire_time_ms(payload.wire.size()), silence,
             payload.note);

    // FR-SCH-08. Loud, and only once: a periodic error line would bury the log
    // it is trying to qualify, but a silent one lets the rig go on producing
    // bursts the reader merges — which looks like a decode fault at the far end
    // and gets debugged there, wrongly.
    static bool complained = false;
    if (silence < e450::MIN_SILENCE_MS && !complained) {
        complained = true;
        ESP_LOGE(TAG,
                 "only %" PRId32 " ms of silence between emissions, want >= %" PRId32 " ms: "
                 "the reader ends a transmission after %" PRId32 " ms of quiet, so consecutive "
                 "emissions may be read as one. Raise SIM_PUSH_PERIOD_SECONDS.",
                 silence, e450::MIN_SILENCE_MS, e450::DUT_BURST_TIMEOUT_MS);
    }
}

[[noreturn]] void scheduler_task(void *) {
    int64_t last_us = esp_timer_get_time();
    int last_emitted_second = -1;

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
        {
            Lock held;
            const e450::DateTime now = g_clock.now();
            mode = g_mode;
            if ((now.second % PUSH_PERIOD_SECONDS == 0) && (now.second != last_emitted_second)) {
                last_emitted_second = now.second;
                due = true;

                if (mode == e450::Mode::E450) {
                    // FR-MOD-04, and the order matters. Choosing the load before
                    // integrating means the energy this telegram reports is
                    // exactly the power this telegram reports, multiplied by the
                    // period — so a reader can check the arithmetic against a
                    // single message, with no reference to what came before.
                    e450::apply_random_load(g_model, g_rng);
                    g_model.tick(PUSH_PERIOD_SECONDS * 1000);  // FR-SCH-03
                }
            }
        }

        if (due) emit(mode);
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
    if (end == argv[1] || *end != 0 || which < 1 || which > 3) {
        std::printf("ERR usage: mode [1|2|3]  (1 pattern, 2 simple, 3 e450)\n");
        return 1;
    }

    const auto mode = static_cast<e450::Mode>(which);
    {
        Lock held;
        g_mode = mode;
    }
    // Takes effect at the next period boundary, not immediately: an emission
    // already on the wire is left to finish, because truncating it would hand
    // the reader a malformed frame and a fault that is not real.
    std::printf("OK %ld %s\n", which, mode_name(mode));
    return 0;
}

// FR-CLI-04: one machine-parsable line, so a test script can assert on it
// without a parser.
int cmd_status(int, char **) {
    Lock held;
    const e450::DateTime now = g_clock.now();
    // PRIu32 rather than %u: uint32_t is `long unsigned int` on Xtensa, and
    // -Werror=format catches the mismatch that would be silent on the host.
    std::printf(
        "OK pi=%" PRIu32 " po=%" PRIu32 " pf=%.2f%s ei=%" PRIu32 " eo=%" PRIu32 " q1=%" PRIu32
        " q2=%" PRIu32 " q3=%" PRIu32 " q4=%" PRIu32 " i=%" PRIu32
        " time=%04u-%02u-%02uT%02u:%02u:%02u mode=%d/%s period=%d emissions=%" PRIu32 "\n",
        g_model.import_w(), g_model.export_w(), static_cast<double>(g_model.power_factor()),
        g_model.lagging() ? "" : "L", g_model.import_wh(), g_model.export_wh(),
        g_model.reactive_varh(e450::ValueModel::Q1_IMPORT_LAGGING),
        g_model.reactive_varh(e450::ValueModel::Q2_EXPORT_LAGGING),
        g_model.reactive_varh(e450::ValueModel::Q3_EXPORT_LEADING),
        g_model.reactive_varh(e450::ValueModel::Q4_IMPORT_LEADING), g_model.current_ma(), now.year,
        now.month, now.day, now.hour, now.minute, now.second, static_cast<int>(g_mode),
        mode_name(g_mode), PUSH_PERIOD_SECONDS, g_emissions);
    return 0;
}

struct CommandDef {
    const char *command;
    const char *help;
    esp_console_cmd_func_t func;
};

void register_commands() {
    static constexpr CommandDef DEFS[] = {
        {"mode", "mode [1|2|3] - 1 pattern, 2 simple telegram, 3 full E450", &cmd_mode},
        {"status", "print the whole state as one key=value line", &cmd_status},
        {"set", "set time <YYYY-MM-DDTHH:MM:SS>", &cmd_set},
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

    ESP_LOGI(TAG, "E450 simulator: mode %d (%s), every %d s, 2400 8E1%s",
             static_cast<int>(g_mode), mode_name(g_mode), PUSH_PERIOD_SECONDS,
             TX_INVERTED ? " inverted" : "");
    ESP_LOGI(TAG, "modes: 1 pattern (is there a signal?), 2 simple (can it decode?), "
                  "3 e450 (can it decode the real thing?)");

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

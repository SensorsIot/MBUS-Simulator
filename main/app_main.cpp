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

constexpr uart_port_t METER_UART = UART_NUM_2;
constexpr auto TX_GPIO = static_cast<gpio_num_t>(CONFIG_SIM_MBUS_TX_GPIO);
constexpr int DUT_POWER_GPIO = CONFIG_SIM_DUT_POWER_GPIO;

// FR-SCH-01: emit when the clock's seconds field is divisible by 5.
constexpr int PUSH_PERIOD_SECONDS = 5;

// How often the scheduler task wakes to re-read the wall clock. Fine enough that
// a 5 s boundary is never missed, coarse enough to cost nothing.
constexpr TickType_t SCHEDULER_POLL_MS = 50;

e450::ValueModel g_model;
e450::SimClock g_clock{e450::DateTime{}};
SemaphoreHandle_t g_lock = nullptr;
bool g_paused = false;
bool g_dut_powered = true;
bool g_burst_requested = false;
uint32_t g_invoke_id = 0;
uint32_t g_telegrams = 0;

struct Lock {
    Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_lock); }
};

// ------------------------------------------------------------------ hardware

void configure_meter_link() {
    // FR-LNK-05, and the one that bites if skipped. With an inverting driver the
    // UART's idle state drives this pin LOW to put 24 V — the mark state — on
    // the bus. Between reset and uart_set_pin() the pin is a floating input, so
    // without driving it here the bus level is undefined on every reboot and the
    // reader sees a break condition or noise instead of a quiet line.
    gpio_config_t idle = {};
    idle.pin_bit_mask = 1ULL << TX_GPIO;
    idle.mode = GPIO_MODE_OUTPUT;
    idle.pull_up_en = GPIO_PULLUP_DISABLE;
    idle.pull_down_en = GPIO_PULLDOWN_DISABLE;
    idle.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&idle));
    gpio_set_level(TX_GPIO, CONFIG_SIM_TX_INVERT ? 0 : 1);

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
#if CONFIG_SIM_TX_INVERT
    ESP_ERROR_CHECK(uart_set_line_inverse(METER_UART, UART_SIGNAL_TXD_INV));
#endif
}

bool dut_power_available() { return DUT_POWER_GPIO >= 0; }

void configure_dut_power() {
    if (!dut_power_available()) {
        ESP_LOGW(TAG, "no DUT power GPIO configured; 'dut on/off' will report ERR");
        return;
    }
    const auto pin = static_cast<gpio_num_t>(DUT_POWER_GPIO);
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(pin, CONFIG_SIM_DUT_POWER_ACTIVE_HIGH ? 1 : 0);
}

void set_dut_power(bool on) {
    if (!dut_power_available()) return;
    const int level = CONFIG_SIM_DUT_POWER_ACTIVE_HIGH ? (on ? 1 : 0) : (on ? 0 : 1);
    gpio_set_level(static_cast<gpio_num_t>(DUT_POWER_GPIO), level);
}

// ----------------------------------------------------------------- scheduler

void transmit_telegram() {
    std::vector<e450::TelegramValue> values;
    e450::TelegramSpec spec;
    uint32_t invoke_id;
    e450::DateTime when;

    {
        Lock held;
        values = e450::ebl::ci1_values(g_model);
        invoke_id = g_invoke_id++;
        when = g_clock.now();
        ++g_telegrams;
    }

    spec.objects = e450::ebl::ci1_objects();
    spec.values = values;
    spec.invoke_id = invoke_id;
    spec.timestamp = when;

    const std::vector<uint8_t> wire = e450::encode_telegram(spec);

    // Blocking, deliberately. At 2400 baud a telegram takes about 1.6 s, and the
    // next tick is 5 s away — but more importantly this is what makes FR-SCH-06
    // true by construction: one task owns the line, so two telegrams can never
    // interleave on it. The clock is driven from esp_timer deltas, not from this
    // task's progress, so blocking here costs no time.
    uart_write_bytes(METER_UART, wire.data(), wire.size());
    uart_wait_tx_done(METER_UART, portMAX_DELAY);
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

        bool emit = false;
        {
            Lock held;
            const e450::DateTime now = g_clock.now();
            const bool on_boundary =
                (now.second % PUSH_PERIOD_SECONDS == 0) && (now.second != last_emitted_second);
            if (g_burst_requested) {
                // FR-SCH-05: a forced burst must not shift the schedule, so it
                // deliberately does not touch last_emitted_second.
                g_burst_requested = false;
                emit = true;
            } else if (on_boundary && !g_paused) {
                last_emitted_second = now.second;
                emit = true;
                // FR-SCH-03/04: integrate once per emission, and not at all
                // while paused.
                g_model.tick(PUSH_PERIOD_SECONDS * 1000);
            }
        }

        if (emit) transmit_telegram();
        vTaskDelay(pdMS_TO_TICKS(SCHEDULER_POLL_MS));
    }
}

// ------------------------------------------------------------------- console

int cmd_set(int argc, char **argv) {
    if (argc < 3) {
        std::printf("ERR usage: set pi|po|pf|time <value>\n");
        return 1;
    }
    const char *what = argv[1];
    Lock held;

    if (std::strcmp(what, "pi") == 0 || std::strcmp(what, "po") == 0) {
        char *end = nullptr;
        const long watts = std::strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || watts < 0 || watts > 1'000'000) {
            std::printf("ERR power must be 0..1000000 W\n");
            return 1;
        }
        if (std::strcmp(what, "pi") == 0) {
            g_model.set_import_w(static_cast<uint32_t>(watts));
        } else {
            g_model.set_export_w(static_cast<uint32_t>(watts));
        }
        std::printf("OK\n");
        return 0;
    }

    if (std::strcmp(what, "pf") == 0) {
        char *end = nullptr;
        const float pf = std::strtof(argv[2], &end);
        if (end == argv[2] || pf < 0.0F || pf > 1.0F) {
            std::printf("ERR power factor must be 0.00..1.00\n");
            return 1;
        }
        const bool lagging = (argc < 4) || (std::strcmp(argv[3], "leading") != 0);
        g_model.set_power_factor(pf, lagging);
        std::printf("OK\n");
        return 0;
    }

    if (std::strcmp(what, "time") == 0) {
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
        g_clock.set(dt);
        std::printf("OK\n");
        return 0;
    }

    std::printf("ERR unknown field '%s'\n", what);
    return 1;
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
        " time=%04u-%02u-%02uT%02u:%02u:%02u paused=%d dut=%d telegrams=%" PRIu32 "\n",
        g_model.import_w(), g_model.export_w(), static_cast<double>(g_model.power_factor()),
        g_model.lagging() ? "" : "L", g_model.import_wh(), g_model.export_wh(),
        g_model.reactive_varh(e450::ValueModel::Q1_IMPORT_LAGGING),
        g_model.reactive_varh(e450::ValueModel::Q2_EXPORT_LAGGING),
        g_model.reactive_varh(e450::ValueModel::Q3_EXPORT_LEADING),
        g_model.reactive_varh(e450::ValueModel::Q4_IMPORT_LEADING), g_model.current_ma(), now.year,
        now.month, now.day, now.hour, now.minute, now.second, g_paused ? 1 : 0,
        g_dut_powered ? 1 : 0, g_telegrams);
    return 0;
}

int cmd_burst(int, char **) {
    Lock held;
    g_burst_requested = true;
    std::printf("OK\n");
    return 0;
}

int cmd_pause(int, char **) {
    Lock held;
    g_paused = true;
    std::printf("OK\n");
    return 0;
}

int cmd_resume(int, char **) {
    Lock held;
    g_paused = false;
    std::printf("OK\n");
    return 0;
}

int cmd_dut(int argc, char **argv) {
    if (!dut_power_available()) {
        std::printf("ERR no DUT power GPIO configured\n");
        return 1;
    }
    if (argc < 2 || (std::strcmp(argv[1], "on") != 0 && std::strcmp(argv[1], "off") != 0)) {
        std::printf("ERR usage: dut on|off\n");
        return 1;
    }
    const bool on = std::strcmp(argv[1], "on") == 0;
    set_dut_power(on);
    {
        Lock held;
        g_dut_powered = on;
    }
    std::printf("OK\n");
    return 0;
}

struct CommandDef {
    const char *command;
    const char *help;
    esp_console_cmd_func_t func;
};

void register_commands() {
    static constexpr CommandDef DEFS[] = {
        {"set", "set pi|po <W> | pf <0..1> [leading] | time <YYYY-MM-DDTHH:MM:SS>", &cmd_set},
        {"status", "print the whole model state as one key=value line", &cmd_status},
        {"burst", "emit one telegram now, without disturbing the schedule", &cmd_burst},
        {"pause", "stop emitting and stop integrating", &cmd_pause},
        {"resume", "resume emitting", &cmd_resume},
        {"dut", "dut on|off - switch the 5 V rail the device under test runs on", &cmd_dut},
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
    g_clock.set(parse_epoch());

    configure_meter_link();
    configure_dut_power();

    ESP_LOGI(TAG, "E450 simulator: EBL Consumer Information 1, 13 objects, %d s, 2400 8E1%s",
             PUSH_PERIOD_SECONDS, CONFIG_SIM_TX_INVERT ? " inverted" : "");

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

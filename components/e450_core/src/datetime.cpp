#include "e450/datetime.h"

namespace e450 {
namespace {

// Howard Hinnant's days_from_civil / civil_from_days. Proleptic Gregorian, valid
// far beyond any date this device will ever hold, and — the reason for using it
// rather than <ctime> — it depends on no C library, no timezone database and no
// global state, so it behaves identically on the host and on the ESP32.
constexpr int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

struct Civil {
    int64_t y;
    unsigned m;
    unsigned d;
};

constexpr Civil civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    return {y + (m <= 2), m, d};
}

constexpr int64_t SECONDS_PER_DAY = 86400;

}  // namespace

uint8_t weekday_of(const DateTime &dt) {
    const int64_t days = days_from_civil(dt.year, dt.month, dt.day);
    // 1970-01-01 was a Thursday, which is 4 in COSEM's Monday-is-1 numbering.
    // The +7 keeps the modulus non-negative for dates before the epoch.
    return static_cast<uint8_t>(((days % 7) + 7 + 3) % 7 + 1);
}

void encode_datetime(const DateTime &dt, std::span<uint8_t, 12> out) {
    out[0] = static_cast<uint8_t>(dt.year >> 8);
    out[1] = static_cast<uint8_t>(dt.year & 0xFF);
    out[2] = dt.month;
    out[3] = dt.day;
    out[4] = weekday_of(dt);
    out[5] = dt.hour;
    out[6] = dt.minute;
    out[7] = dt.second;
    out[8] = 0xFF;  // hundredths — not specified
    out[9] = 0x80;  // deviation — not specified
    out[10] = 0x00;
    out[11] = 0x00;  // clock status — valid
}

void SimClock::set(const DateTime &dt) {
    seconds_ = days_from_civil(dt.year, dt.month, dt.day) * SECONDS_PER_DAY +
               dt.hour * 3600 + dt.minute * 60 + dt.second;
    millis_ = 0;
}

void SimClock::advance_ms(uint32_t ms) {
    millis_ += ms;
    seconds_ += millis_ / 1000;
    millis_ %= 1000;
}

DateTime SimClock::now() const {
    const int64_t days = seconds_ / SECONDS_PER_DAY -
                         ((seconds_ % SECONDS_PER_DAY < 0) ? 1 : 0);
    int64_t sod = seconds_ - days * SECONDS_PER_DAY;
    const Civil c = civil_from_days(days);

    DateTime dt;
    dt.year = static_cast<uint16_t>(c.y);
    dt.month = static_cast<uint8_t>(c.m);
    dt.day = static_cast<uint8_t>(c.d);
    dt.hour = static_cast<uint8_t>(sod / 3600);
    dt.minute = static_cast<uint8_t>((sod % 3600) / 60);
    dt.second = static_cast<uint8_t>(sod % 60);
    return dt;
}

}  // namespace e450

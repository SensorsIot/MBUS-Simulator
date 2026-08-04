#pragma once

#include <cstdint>
#include <span>

namespace e450 {

// COSEM date-time, the 12-byte octet string that heads every DataNotification.
// FSD FR-ENC-11 / Appendix B.
struct DateTime {
    uint16_t year = 2026;
    uint8_t month = 1;   // 1..12
    uint8_t day = 1;     // 1..31
    uint8_t hour = 0;    // 0..23
    uint8_t minute = 0;  // 0..59
    uint8_t second = 0;  // 0..59
};

// Day of week, 1 = Monday .. 7 = Sunday, as COSEM numbers it. Derived rather
// than stored: a stored weekday is a second source of truth that can disagree
// with the date, and the captures show the meter's is always consistent.
uint8_t weekday_of(const DateTime &dt);

// Encode to the 12-byte wire form:
//   YY YY MM DD DOW HH MM SS 00(hundredths=FF) DEV(2) STATUS
// Hundredths 0xFF (unspecified), deviation 0x8000 (unspecified) and status 0x00
// (valid) are what both real captures carry — FR-ENC-11.
void encode_datetime(const DateTime &dt, std::span<uint8_t, 12> out);

// A calendar clock advanced by elapsed milliseconds and nothing else — no NTP,
// no RTC, no drift (FR-CLK-01, FR-CLK-03). Determinism is the point: the same
// commands from reset must produce the same bytes (FR-CLK-04).
class SimClock {
  public:
    explicit SimClock(const DateTime &epoch) { set(epoch); }

    void set(const DateTime &dt);
    void advance_ms(uint32_t ms);

    [[nodiscard]] DateTime now() const;

    // Seconds since the internal epoch — used by the scheduler to find the 5 s
    // boundaries without converting to a calendar first.
    [[nodiscard]] int64_t unix_seconds() const { return seconds_; }

  private:
    int64_t seconds_ = 0;
    uint32_t millis_ = 0;  // sub-second remainder, 0..999
};

}  // namespace e450

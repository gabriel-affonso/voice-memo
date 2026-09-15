#pragma once

// UTC <-> local conversions for the UI clock.
//
// The PCF85063 has no notion of a timezone, so the firmware keeps it on UTC and
// converts to Europe/Lisbon only when the top bar is painted. The conversion is
// delegated to the C library: applyPosixTimezone() installs the POSIX TZ rule
// (for mainland Portugal: WET0WEST,M3.5.0/1,M10.5.0/2) through setenv()/tzset(),
// and localtime_r() then applies the right WET/WEST offset and the official DST
// transitions for the date being converted. No fixed offset and no manual
// "+1 hour" exists anywhere.
//
// This header is deliberately free of Arduino, WiFi and I2C, so
// tests/time_zone_test.cpp can exercise the exact conversion the board runs -
// including both DST transitions - on the Mac.

#include <cstdint>

namespace voice_memo_time {

// Broken-down UTC calendar. This is exactly what the PCF85063 stores.
struct UtcDateTime {
    int year = 1970;   // full year; the chip stores yy, so the range is 2000..2099
    int month = 1;     // 1..12
    int day = 1;       // 1..31
    int hour = 0;      // 0..23
    int minute = 0;    // 0..59
    int second = 0;    // 0..59
};

// Broken-down local calendar, plus the DST flag for the serial log.
struct LocalDateTime {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    bool dst = false;
};

// Plausibility window of a real wall-clock instant, in seconds since the Unix
// epoch. Anything outside it is either the 1970 cold start or a never-set RTC,
// never a time the UI is allowed to show.
//   2025-01-01T00:00:00Z .. 2100-01-01T00:00:00Z
constexpr int64_t kMinTrustworthyUtcEpoch = 1735689600LL;
constexpr int64_t kMaxTrustworthyUtcEpoch = 4102444800LL;

bool isLeapYear(int year);
int daysInMonth(int year, int month);
bool isValidUtcDateTime(const UtcDateTime& value);

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Pure integer arithmetic, so the ESP32 and the Mac agree and
// no libc calendar is needed.
int64_t daysFromCivil(int year, int month, int day);
void civilFromDays(int64_t days, int* year, int* month, int* day);

// Seconds since 1970-01-01T00:00:00Z. Returns 0 for an invalid calendar.
int64_t utcToEpoch(const UtcDateTime& value);
UtcDateTime epochToUtc(int64_t epoch);

// true when `epoch` lies inside the plausibility window above.
bool isTrustworthyUtcEpoch(int64_t epoch);

// The two candidate UTC sources the UI may display. A field marked valid is
// only read when it is set.
struct ClockInputs {
    bool rtc_valid = false;        // a trustworthy PCF85063 reading
    int64_t rtc_epoch_utc = 0;
    bool system_valid = false;     // an NTP-disciplined system clock
    int64_t system_epoch_utc = 0;
};

// Picks the UTC instant the UI must show: the RTC first (it is the offline
// source of truth), the NTP system clock as the fallback for a missing or dead
// RTC, and nothing at all otherwise. Returns false - and writes 0 - exactly
// when neither source is trustworthy, which is the "--:--" case.
bool selectClockUtc(const ClockInputs& inputs, int64_t* out_epoch_utc);

// Installs a POSIX TZ rule into the process environment (setenv + tzset).
// Returns false for a null or empty string.
bool applyPosixTimezone(const char* posix_tz);

// localtime_r() under the TZ installed by applyPosixTimezone().
bool epochToLocal(int64_t epoch, LocalDateTime* out);

// local - UTC in seconds at `epoch` (0 in WET, 3600 in WEST). False when the
// instant cannot be converted.
bool localUtcOffsetSeconds(int64_t epoch, int* out_offset_seconds);

// Writes "HH:MM" in local time for `epoch_utc`, or "--:--" when `epoch_utc` is
// not a trustworthy instant. Always NUL-terminates inside 6 bytes.
void formatLocalHhMm(int64_t epoch_utc, char out[6]);

}  // namespace voice_memo_time

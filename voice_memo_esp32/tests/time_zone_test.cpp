// Host-side tests for the UTC <-> Europe/Lisbon conversion behind the UI clock.
//
// The PCF85063 holds UTC; the panel shows Europe/Lisbon local time. Everything
// in time_zone.{h,cpp} is Arduino free, so the exact conversion the firmware
// runs can be verified on the Mac, with no board:
//
//     c++ -std=c++11 -Wall -Wextra -o /tmp/time_zone_test \
//         tests/time_zone_test.cpp time_zone.cpp
//     /tmp/time_zone_test
//
// Covered here:
//   * winter (WET, UTC+0) and summer (WEST, UTC+1) mapping on ordinary days
//   * both official DST transitions, to the second, for several years
//   * the "RTC first, NTP system clock second, nothing otherwise" policy
//   * an invalid / never-set RTC -> "--:--" (never a plausible-looking time)
//   * the pure Gregorian arithmetic (leap days) and calendar validation
//
// This directory is not compiled into the firmware: Arduino only builds the
// sketch root and src/, so tests/ is ignored by arduino-cli.

#include <cstdio>
#include <cstring>

#include "../time_zone.h"

namespace {

using namespace voice_memo_time;

int g_failures = 0;
int g_checks = 0;

void checkInt(const char* label, long long actual, long long expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s = %lld\n", label, actual);
        return;
    }
    std::printf("FAIL %s: expected %lld, got %lld\n", label, expected, actual);
    ++g_failures;
}

void checkBool(const char* label, bool actual, bool expected) {
    ++g_checks;
    if (actual == expected) {
        std::printf("PASS %s\n", label);
        return;
    }
    std::printf("FAIL %s: expected %s, got %s\n",
                label, expected ? "true" : "false", actual ? "true" : "false");
    ++g_failures;
}

void checkText(const char* label, const char* actual, const char* expected) {
    ++g_checks;
    if (std::strcmp(actual, expected) == 0) {
        std::printf("PASS %s = \"%s\"\n", label, actual);
        return;
    }
    std::printf("FAIL %s: expected \"%s\", got \"%s\"\n", label, expected, actual);
    ++g_failures;
}

void section(const char* title) {
    std::printf("\n--- %s\n", title);
}

UtcDateTime utc(int year, int month, int day, int hour, int minute, int second) {
    UtcDateTime value;
    value.year = year;
    value.month = month;
    value.day = day;
    value.hour = hour;
    value.minute = minute;
    value.second = second;
    return value;
}

int64_t utcEpoch(int year, int month, int day, int hour, int minute, int second) {
    return utcToEpoch(utc(year, month, day, hour, minute, second));
}

// Asserts the full local calendar for one UTC instant.
void checkLocal(
    const char* label,
    int64_t epoch,
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second,
    bool dst
) {
    LocalDateTime local;
    ++g_checks;
    if (!epochToLocal(epoch, &local)) {
        std::printf("FAIL %s: epochToLocal() failed\n", label);
        ++g_failures;
        return;
    }
    const bool matches = local.year == year && local.month == month && local.day == day &&
                         local.hour == hour && local.minute == minute && local.second == second &&
                         local.dst == dst;
    if (matches) {
        std::printf("PASS %s -> %04d-%02d-%02d %02d:%02d:%02d %s\n",
                    label, local.year, local.month, local.day, local.hour, local.minute,
                    local.second, local.dst ? "WEST/UTC+1" : "WET/UTC+0");
        return;
    }
    std::printf("FAIL %s: got %04d-%02d-%02d %02d:%02d:%02d dst=%d\n",
                label, local.year, local.month, local.day, local.hour, local.minute, local.second,
                local.dst ? 1 : 0);
    ++g_failures;
}

void checkOffset(const char* label, int64_t epoch, int expected_seconds) {
    int offset = 0;
    ++g_checks;
    if (!localUtcOffsetSeconds(epoch, &offset)) {
        std::printf("FAIL %s: localUtcOffsetSeconds() failed\n", label);
        ++g_failures;
        return;
    }
    if (offset == expected_seconds) {
        std::printf("PASS %s = %d s\n", label, offset);
        return;
    }
    std::printf("FAIL %s: expected %d s, got %d s\n", label, expected_seconds, offset);
    ++g_failures;
}

void checkHhMm(const char* label, int64_t epoch, const char* expected) {
    char text[6];
    formatLocalHhMm(epoch, text);
    checkText(label, text, expected);
}

}  // namespace

int main() {
    // The rule configured in config.h for mainland Portugal (Europe/Lisbon).
    section("timezone installation");
    checkBool("applyPosixTimezone(WET0WEST,M3.5.0/1,M10.5.0/2)", applyPosixTimezone("WET0WEST,M3.5.0/1,M10.5.0/2"), true);
    checkBool("applyPosixTimezone(nullptr) rejected", applyPosixTimezone(nullptr), false);
    checkBool("applyPosixTimezone(\"\") rejected", applyPosixTimezone(""), false);

    // ---------------------------------------------------------------------
    section("UTC winter -> Lisbon (WET, UTC+0)");
    checkLocal("2026-01-15 12:00 UTC", utcEpoch(2026, 1, 15, 12, 0, 0),
               2026, 1, 15, 12, 0, 0, false);
    checkOffset("2026-01-15 offset", utcEpoch(2026, 1, 15, 12, 0, 0), 0);
    checkHhMm("2026-01-15 12:00 UTC shows", utcEpoch(2026, 1, 15, 12, 0, 0), "12:00");

    // ---------------------------------------------------------------------
    section("UTC summer -> Lisbon (WEST, UTC+1)");
    checkLocal("2026-09-15 12:00 UTC", utcEpoch(2026, 9, 15, 12, 0, 0),
               2026, 9, 15, 13, 0, 0, true);
    checkOffset("2026-09-15 offset", utcEpoch(2026, 9, 15, 12, 0, 0), 3600);
    checkHhMm("2026-09-15 12:00 UTC shows", utcEpoch(2026, 9, 15, 12, 0, 0), "13:00");

    // Same UTC hour, different local hour: proof that no fixed offset is used.
    {
        int januaryOffset = 0;
        int septemberOffset = 0;
        localUtcOffsetSeconds(utcEpoch(2026, 1, 15, 12, 0, 0), &januaryOffset);
        localUtcOffsetSeconds(utcEpoch(2026, 9, 15, 12, 0, 0), &septemberOffset);
        checkBool("January and September offsets differ (no fixed offset)",
                  januaryOffset != septemberOffset, true);
    }

    // ---------------------------------------------------------------------
    section("spring transition (last Sunday of March, 01:00 UTC)");
    // 2026-03-29: clocks jump 01:00 WET -> 02:00 WEST.
    checkLocal("2026-03-29 00:59:59 UTC (last WET second)", utcEpoch(2026, 3, 29, 0, 59, 59),
               2026, 3, 29, 0, 59, 59, false);
    checkOffset("2026-03-29 00:59:59 offset", utcEpoch(2026, 3, 29, 0, 59, 59), 0);
    checkLocal("2026-03-29 01:00:00 UTC (first WEST second)", utcEpoch(2026, 3, 29, 1, 0, 0),
               2026, 3, 29, 2, 0, 0, true);
    checkLocal("2026-03-29 01:00:01 UTC", utcEpoch(2026, 3, 29, 1, 0, 1),
               2026, 3, 29, 2, 0, 1, true);
    checkOffset("2026-03-29 01:00:00 offset", utcEpoch(2026, 3, 29, 1, 0, 0), 3600);
    checkHhMm("2026-03-29 01:00 UTC shows", utcEpoch(2026, 3, 29, 1, 0, 0), "02:00");
    checkHhMm("2026-03-29 00:59 UTC shows", utcEpoch(2026, 3, 29, 0, 59, 0), "00:59");

    // 2027-03-28 and 2028-03-26 are the next two spring dates.
    checkLocal("2027-03-28 01:00:00 UTC", utcEpoch(2027, 3, 28, 1, 0, 0),
               2027, 3, 28, 2, 0, 0, true);
    checkLocal("2027-03-28 00:59:59 UTC", utcEpoch(2027, 3, 28, 0, 59, 59),
               2027, 3, 28, 0, 59, 59, false);
    checkLocal("2028-03-26 01:00:00 UTC", utcEpoch(2028, 3, 26, 1, 0, 0),
               2028, 3, 26, 2, 0, 0, true);

    // ---------------------------------------------------------------------
    section("autumn transition (last Sunday of October, 01:00 UTC)");
    // 2026-10-25: clocks fall back 02:00 WEST -> 01:00 WET.
    checkLocal("2026-10-25 00:59:59 UTC (last WEST second)", utcEpoch(2026, 10, 25, 0, 59, 59),
               2026, 10, 25, 1, 59, 59, true);
    checkLocal("2026-10-25 01:00:00 UTC (first WET second)", utcEpoch(2026, 10, 25, 1, 0, 0),
               2026, 10, 25, 1, 0, 0, false);
    checkLocal("2026-10-25 01:00:01 UTC", utcEpoch(2026, 10, 25, 1, 0, 1),
               2026, 10, 25, 1, 0, 1, false);
    checkOffset("2026-10-25 00:59:59 offset", utcEpoch(2026, 10, 25, 0, 59, 59), 3600);
    checkOffset("2026-10-25 01:00:00 offset", utcEpoch(2026, 10, 25, 1, 0, 0), 0);
    checkHhMm("2026-10-25 00:59 UTC shows", utcEpoch(2026, 10, 25, 0, 59, 0), "01:59");
    checkHhMm("2026-10-25 01:00 UTC shows", utcEpoch(2026, 10, 25, 1, 0, 0), "01:00");

    // 2027-10-31 and 2028-10-29 are the next two autumn dates.
    checkLocal("2027-10-31 01:00:00 UTC", utcEpoch(2027, 10, 31, 1, 0, 0),
               2027, 10, 31, 1, 0, 0, false);
    checkLocal("2027-10-31 00:59:59 UTC", utcEpoch(2027, 10, 31, 0, 59, 59),
               2027, 10, 31, 1, 59, 59, true);
    checkLocal("2028-10-29 01:00:00 UTC", utcEpoch(2028, 10, 29, 1, 0, 0),
               2028, 10, 29, 1, 0, 0, false);

    // Midnight boundaries: the local hour can cross into another day.
    checkHhMm("2026-01-15 23:59 UTC shows", utcEpoch(2026, 1, 15, 23, 59, 0), "23:59");
    checkHhMm("2026-09-15 22:30 UTC shows", utcEpoch(2026, 9, 15, 22, 30, 0), "23:30");
    checkLocal("2026-09-15 23:30 UTC rolls to the next day", utcEpoch(2026, 9, 15, 23, 30, 0),
               2026, 9, 16, 0, 30, 0, true);

    // ---------------------------------------------------------------------
    section("invalid RTC -> \"--:--\"");
    {
        char text[6];
        formatLocalHhMm(0, text);
        checkText("epoch 0 (never set) -> --:--", text, "--:--");
        formatLocalHhMm(-1, text);
        checkText("negative epoch -> --:--", text, "--:--");
        formatLocalHhMm(kMinTrustworthyUtcEpoch - 1, text);
        checkText("just below the plausibility window -> --:--", text, "--:--");
        formatLocalHhMm(kMaxTrustworthyUtcEpoch, text);
        checkText("just above the plausibility window -> --:--", text, "--:--");
    }

    // ---------------------------------------------------------------------
    section("clock source policy: RTC first, NTP fallback, else nothing");
    {
        const int64_t rtcEpoch = utcEpoch(2026, 1, 15, 12, 0, 0);
        const int64_t ntpEpoch = utcEpoch(2026, 9, 15, 12, 0, 0);
        int64_t chosen = -12345;

        ClockInputs inputs;
        // Both sources dead: this is the invalid-RTC, still-offline boot.
        checkBool("neither source -> no trustworthy time",
                  selectClockUtc(inputs, &chosen), false);
        checkInt("neither source -> epoch forced to 0", chosen, 0);
        {
            char text[6];
            formatLocalHhMm(chosen, text);
            checkText("invalid RTC + no NTP -> --:--", text, "--:--");
        }

        // RTC present and valid: it wins even when NTP also has an answer.
        inputs.rtc_valid = true;
        inputs.rtc_epoch_utc = rtcEpoch;
        checkBool("valid RTC -> trustworthy", selectClockUtc(inputs, &chosen), true);
        checkInt("valid RTC -> RTC instant wins", chosen, rtcEpoch);

        inputs.system_valid = true;
        inputs.system_epoch_utc = ntpEpoch;
        checkBool("valid RTC beats the NTP clock", selectClockUtc(inputs, &chosen), true);
        checkInt("valid RTC beats the NTP clock (epoch)", chosen, rtcEpoch);

        // RTC absent/invalid, NTP valid: the system clock is the fallback.
        inputs.rtc_valid = false;
        inputs.rtc_epoch_utc = 0;
        checkBool("invalid RTC + valid NTP -> trustworthy", selectClockUtc(inputs, &chosen), true);
        checkInt("invalid RTC + valid NTP -> system instant", chosen, ntpEpoch);

        // A "valid" flag pointing at a cold-start epoch is still rejected.
        inputs.rtc_valid = true;
        inputs.rtc_epoch_utc = 0;
        inputs.system_valid = false;
        inputs.system_epoch_utc = 0;
        checkBool("RTC claiming epoch 0 is rejected", selectClockUtc(inputs, &chosen), false);
        checkInt("rejected RTC forces epoch 0", chosen, 0);
    }

    // ---------------------------------------------------------------------
    section("Gregorian arithmetic and validation");
    checkBool("2000 is a leap year", isLeapYear(2000), true);
    checkBool("2026 is not a leap year", isLeapYear(2026), false);
    checkBool("2028 is a leap year", isLeapYear(2028), true);
    checkBool("1900 is not a leap year", isLeapYear(1900), false);
    checkInt("days in 2028-02", daysInMonth(2028, 2), 29);
    checkInt("days in 2026-02", daysInMonth(2026, 2), 28);
    checkInt("days in 2026-04", daysInMonth(2026, 4), 30);

    checkBool("2026-02-29 rejected", isValidUtcDateTime(utc(2026, 2, 29, 0, 0, 0)), false);
    checkBool("2028-02-29 accepted", isValidUtcDateTime(utc(2028, 2, 29, 0, 0, 0)), true);
    checkBool("month 13 rejected", isValidUtcDateTime(utc(2026, 13, 1, 0, 0, 0)), false);
    checkBool("month 0 rejected", isValidUtcDateTime(utc(2026, 0, 1, 0, 0, 0)), false);
    checkBool("day 0 rejected", isValidUtcDateTime(utc(2026, 5, 0, 0, 0, 0)), false);
    checkBool("2026-04-31 rejected", isValidUtcDateTime(utc(2026, 4, 31, 0, 0, 0)), false);
    checkBool("hour 24 rejected", isValidUtcDateTime(utc(2026, 5, 1, 24, 0, 0)), false);
    checkBool("minute 60 rejected", isValidUtcDateTime(utc(2026, 5, 1, 0, 60, 0)), false);
    checkBool("second 60 rejected", isValidUtcDateTime(utc(2026, 5, 1, 0, 0, 60)), false);
    checkBool("1970-01-01 00:00:00 accepted", isValidUtcDateTime(utc(1970, 1, 1, 0, 0, 0)), true);
    checkInt("invalid calendar maps to epoch 0", utcToEpoch(utc(2026, 2, 29, 12, 0, 0)), 0);

    // round trips
    const int64_t samples[] = {
        utcEpoch(1970, 1, 1, 0, 0, 0),
        utcEpoch(2000, 2, 29, 12, 34, 56),
        utcEpoch(2026, 1, 1, 0, 0, 0),
        utcEpoch(2026, 12, 31, 23, 59, 59),
        utcEpoch(2028, 2, 29, 6, 0, 0),
        utcEpoch(2099, 12, 31, 23, 59, 59),
    };
    bool roundTripOk = true;
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        const UtcDateTime back = epochToUtc(samples[i]);
        if (utcToEpoch(back) != samples[i]) {
            roundTripOk = false;
        }
    }
    checkBool("epoch -> calendar -> epoch round-trips", roundTripOk, true);

    const UtcDateTime known = utc(2026, 1, 15, 12, 0, 0);
    checkInt("2026-01-15 12:00 UTC epoch", utcToEpoch(known), 1768478400LL);

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("all time_zone tests passed\n");
        return 0;
    }
    std::printf("%d time_zone test(s) FAILED\n", g_failures);
    return 1;
}

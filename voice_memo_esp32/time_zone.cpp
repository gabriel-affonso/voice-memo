#include "time_zone.h"

#include <cstdlib>
#include <ctime>

namespace voice_memo_time {
namespace {

constexpr int64_t kSecondsPerDay = 86400LL;

// Floor division, so negative epochs (before 1970) still map to the correct
// calendar day instead of truncating towards zero.
int64_t floorDiv(int64_t value, int64_t divisor) {
    const int64_t quotient = value / divisor;
    const int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        return quotient - 1;
    }
    return quotient;
}

bool validTimeOfDay(const UtcDateTime& value) {
    return value.hour >= 0 && value.hour <= 23 && value.minute >= 0 && value.minute <= 59 &&
           value.second >= 0 && value.second <= 59;
}

}  // namespace

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int daysInMonth(int year, int month) {
    switch (month) {
        case 1:
        case 3:
        case 5:
        case 7:
        case 8:
        case 10:
        case 12:
            return 31;
        case 4:
        case 6:
        case 9:
        case 11:
            return 30;
        case 2:
            return isLeapYear(year) ? 29 : 28;
        default:
            return 0;
    }
}

bool isValidUtcDateTime(const UtcDateTime& value) {
    if (value.year < 1970 || value.year > 2199) {
        return false;
    }
    if (value.month < 1 || value.month > 12) {
        return false;
    }
    const int lastDay = daysInMonth(value.year, value.month);
    if (value.day < 1 || value.day > lastDay) {
        return false;
    }
    return validTimeOfDay(value);
}

// Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms":
// days since 1970-01-01 for a proleptic Gregorian y/m/d.
int64_t daysFromCivil(int year, int month, int day) {
    int64_t y = year;
    const int64_t m = month;
    const int64_t d = day;
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                        // [0, 399]
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;       // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                // [0, 146096]
    return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t days, int* year, int* month, int* day) {
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                                     // [0, 146096]
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              // [0, 365]
    const int64_t mp = (5 * doy + 2) / 153;                                   // [0, 11]
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;                           // [1, 31]
    const int64_t m = mp + (mp < 10 ? 3 : -9);                                // [1, 12]

    if (year != nullptr) {
        *year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    }
    if (month != nullptr) {
        *month = static_cast<int>(m);
    }
    if (day != nullptr) {
        *day = static_cast<int>(d);
    }
}

int64_t utcToEpoch(const UtcDateTime& value) {
    if (!isValidUtcDateTime(value)) {
        return 0;
    }
    const int64_t days = daysFromCivil(value.year, value.month, value.day);
    return days * kSecondsPerDay + value.hour * 3600LL + value.minute * 60LL + value.second;
}

UtcDateTime epochToUtc(int64_t epoch) {
    UtcDateTime out;
    const int64_t days = floorDiv(epoch, kSecondsPerDay);
    const int64_t secondsOfDay = epoch - days * kSecondsPerDay;

    int year = 1970;
    int month = 1;
    int day = 1;
    civilFromDays(days, &year, &month, &day);

    out.year = year;
    out.month = month;
    out.day = day;
    out.hour = static_cast<int>(secondsOfDay / 3600);
    out.minute = static_cast<int>((secondsOfDay % 3600) / 60);
    out.second = static_cast<int>(secondsOfDay % 60);
    return out;
}

bool isTrustworthyUtcEpoch(int64_t epoch) {
    return epoch >= kMinTrustworthyUtcEpoch && epoch < kMaxTrustworthyUtcEpoch;
}

bool selectClockUtc(const ClockInputs& inputs, int64_t* out_epoch_utc) {
    if (out_epoch_utc == nullptr) {
        return false;
    }

    if (inputs.rtc_valid && isTrustworthyUtcEpoch(inputs.rtc_epoch_utc)) {
        *out_epoch_utc = inputs.rtc_epoch_utc;
        return true;
    }
    if (inputs.system_valid && isTrustworthyUtcEpoch(inputs.system_epoch_utc)) {
        *out_epoch_utc = inputs.system_epoch_utc;
        return true;
    }

    *out_epoch_utc = 0;
    return false;
}

bool applyPosixTimezone(const char* posix_tz) {
    if (posix_tz == nullptr || posix_tz[0] == '\0') {
        return false;
    }
    // setenv() copies the string, so a literal from config.h is fine.
    setenv("TZ", posix_tz, 1);
    tzset();
    return true;
}

bool epochToLocal(int64_t epoch, LocalDateTime* out) {
    if (out == nullptr) {
        return false;
    }
    const time_t seconds = static_cast<time_t>(epoch);
    struct tm local = {};
    if (localtime_r(&seconds, &local) == nullptr) {
        return false;
    }

    out->year = local.tm_year + 1900;
    out->month = local.tm_mon + 1;
    out->day = local.tm_mday;
    out->hour = local.tm_hour;
    out->minute = local.tm_min;
    out->second = local.tm_sec;
    out->dst = local.tm_isdst > 0;
    return true;
}

bool localUtcOffsetSeconds(int64_t epoch, int* out_offset_seconds) {
    if (out_offset_seconds == nullptr) {
        return false;
    }
    LocalDateTime local;
    if (!epochToLocal(epoch, &local)) {
        return false;
    }

    // Reinterpreting the local fields as UTC and subtracting the real instant
    // yields the offset without relying on tm_gmtoff, which is not portable to
    // every newlib configuration.
    UtcDateTime asIfUtc;
    asIfUtc.year = local.year;
    asIfUtc.month = local.month;
    asIfUtc.day = local.day;
    asIfUtc.hour = local.hour;
    asIfUtc.minute = local.minute;
    asIfUtc.second = local.second;

    *out_offset_seconds = static_cast<int>(utcToEpoch(asIfUtc) - epoch);
    return true;
}

void formatLocalHhMm(int64_t epoch_utc, char out[6]) {
    LocalDateTime local;
    if (!isTrustworthyUtcEpoch(epoch_utc) || !epochToLocal(epoch_utc, &local)) {
        out[0] = '-';
        out[1] = '-';
        out[2] = ':';
        out[3] = '-';
        out[4] = '-';
        out[5] = '\0';
        return;
    }

    out[0] = static_cast<char>('0' + (local.hour / 10) % 10);
    out[1] = static_cast<char>('0' + local.hour % 10);
    out[2] = ':';
    out[3] = static_cast<char>('0' + (local.minute / 10) % 10);
    out[4] = static_cast<char>('0' + local.minute % 10);
    out[5] = '\0';
}

}  // namespace voice_memo_time

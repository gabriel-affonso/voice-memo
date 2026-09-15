#include "time_manager.h"

#include <time.h>

#include "config.h"

namespace voice_memo_ui {
namespace {

// Signed-difference comparisons, so both helpers survive the millis() wrap.
bool deadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t elapsedSince(uint32_t now, uint32_t start) {
    return now - start;
}

// SNTP keeps the system clock on UTC through settimeofday(); the TZ only
// affects localtime_r(), never this value.
int64_t systemUtcEpoch() {
    return static_cast<int64_t>(time(nullptr));
}

}  // namespace

TimeManager::TimeManager(RtcPcf85063& rtc) : rtc_(rtc) {}

void TimeManager::begin() {
    if (!voice_memo_time::applyPosixTimezone(VM_TIME_TZ)) {
        Serial.println("[time] empty timezone string; local time unavailable");
    }
    Serial.printf("[time] timezone: %s (Europe/Lisbon, WET/WEST)\n", VM_TIME_TZ);

    if (!rtc_.begin()) {
        Serial.println("[time] RTC not usable; the UI uses NTP when it is available");
    }

    pollRtc();
    logClock();
}

void TimeManager::update(bool wifi_connected) {
    const uint32_t now = millis();

    serviceNtp(wifi_connected, now);

    if (!deadlineReached(now, nextRtcPollMs_)) {
        return;
    }
    nextRtcPollMs_ = now + VM_UI_CLOCK_INTERVAL_MS;
    pollRtc();
}

void TimeManager::serviceNtp(bool wifi_connected, uint32_t now) {
    if (!wifi_connected) {
        // Nothing to synchronise against. Forget the "already started" state so
        // the next connection issues exactly one fresh request.
        ntpStarted_ = false;
        return;
    }

    bool restart = !ntpStarted_;
    if (!restart) {
        const uint32_t interval =
            ntpSynced_ ? VM_TIME_NTP_RESYNC_INTERVAL_MS : VM_TIME_NTP_RETRY_INTERVAL_MS;
        restart = elapsedSince(now, ntpStartedMs_) >= interval;
    }

    if (restart) {
        // configTzTime() only (re)starts the SNTP client and installs the same
        // POSIX TZ rule; it returns immediately, so NTP never blocks the loop.
        configTzTime(VM_TIME_TZ, VM_TIME_NTP_SERVER_1, VM_TIME_NTP_SERVER_2);
        ntpStarted_ = true;
        ntpStartedMs_ = now;
        if (!ntpSynced_) {
            Serial.printf("[time] NTP request sent to %s / %s\n",
                          VM_TIME_NTP_SERVER_1,
                          VM_TIME_NTP_SERVER_2);
        }
    }

    const int64_t systemEpoch = systemUtcEpoch();
    if (!voice_memo_time::isTrustworthyUtcEpoch(systemEpoch)) {
        // SNTP has not answered yet: the RTC (or "--:--") keeps the UI honest.
        return;
    }

    if (!ntpSynced_) {
        ntpSynced_ = true;
        // Publish the first NTP-derived time instead of waiting for the poll
        // timer. This is also the only publish path when the RTC is absent.
        nextRtcPollMs_ = 0;
        Serial.println("[time] NTP synchronized");
    }

    syncRtcFromSystem(now, systemEpoch);
}

void TimeManager::syncRtcFromSystem(uint32_t now, int64_t systemEpoch) {
    if (!rtc_.available()) {
        if (!rtcFallbackLogged_) {
            rtcFallbackLogged_ = true;
            Serial.println("[time] no PCF85063; UI clock uses the NTP system time");
        }
        // No chip to discipline: the scheduled poll publishes the system time.
        return;
    }

    bool needWrite = !rtcSyncedFromNtp_;
    if (!needWrite && !rtcValid_) {
        // The chip stopped (backup cell died) after the previous correction.
        needWrite = true;
    }
    if (!needWrite) {
        const int64_t delta = systemEpoch - rtcEpochUtc_;
        needWrite = delta > VM_TIME_RTC_SYNC_TOLERANCE_S || delta < -VM_TIME_RTC_SYNC_TOLERANCE_S;
    }
    if (!needWrite || !deadlineReached(now, nextRtcWriteMs_)) {
        return;
    }

    const voice_memo_time::UtcDateTime utc = voice_memo_time::epochToUtc(systemEpoch);
    if (!rtc_.writeUtc(utc)) {
        Serial.println("[time] RTC write failed; retrying later");
        nextRtcWriteMs_ = now + VM_TIME_NTP_RETRY_INTERVAL_MS;
        return;
    }

    rtcSyncedFromNtp_ = true;
    nextRtcWriteMs_ = 0;
    Serial.printf("[time] RTC updated from NTP (UTC %04d-%02d-%02d %02d:%02d:%02d)\n",
                  utc.year,
                  utc.month,
                  utc.day,
                  utc.hour,
                  utc.minute,
                  utc.second);

    // Publish the correction immediately instead of waiting for the poll timer.
    pollRtc();
    nextRtcPollMs_ = millis() + VM_UI_CLOCK_INTERVAL_MS;
    logClock();
}

void TimeManager::pollRtc() {
    rtcValid_ = false;
    rtcEpochUtc_ = 0;

    if (rtc_.available()) {
        voice_memo_time::UtcDateTime utc;
        if (rtc_.readUtc(&utc)) {
            const int64_t epoch = voice_memo_time::utcToEpoch(utc);
            if (voice_memo_time::isTrustworthyUtcEpoch(epoch)) {
                rtcValid_ = true;
                rtcEpochUtc_ = epoch;
            }
        }
    }

    const int64_t systemEpoch = systemUtcEpoch();
    const bool systemValid = ntpSynced_ && voice_memo_time::isTrustworthyUtcEpoch(systemEpoch);

    // The RTC is the offline source of truth; the NTP-disciplined system clock
    // is only a fallback for a missing or dead RTC. The policy itself is shared
    // with the host tests (time_zone.cpp).
    voice_memo_time::ClockInputs inputs;
    inputs.rtc_valid = rtcValid_;
    inputs.rtc_epoch_utc = rtcEpochUtc_;
    inputs.system_valid = systemValid;
    inputs.system_epoch_utc = systemEpoch;

    valid_ = voice_memo_time::selectClockUtc(inputs, &epochUtc_);
    voice_memo_time::formatLocalHhMm(epochUtc_, text_);

    if (!valid_) {
        if (!loggedInvalid_) {
            loggedInvalid_ = true;
            Serial.println(
                "[time] no trustworthy time (RTC invalid and NTP not synchronized); UI shows --:--");
        }
    } else {
        loggedInvalid_ = false;
    }
}

void TimeManager::logClock() {
    if (!valid_) {
        // pollRtc() has already printed the one-shot "--:--" explanation.
        return;
    }

    const voice_memo_time::UtcDateTime utc = voice_memo_time::epochToUtc(epochUtc_);
    if (rtcValid_) {
        Serial.printf("[time] RTC UTC: %04d-%02d-%02d %02d:%02d:%02d\n",
                      utc.year,
                      utc.month,
                      utc.day,
                      utc.hour,
                      utc.minute,
                      utc.second);
    } else {
        Serial.printf("[time] system UTC (NTP): %04d-%02d-%02d %02d:%02d:%02d\n",
                      utc.year,
                      utc.month,
                      utc.day,
                      utc.hour,
                      utc.minute,
                      utc.second);
    }

    voice_memo_time::LocalDateTime local;
    int offsetSeconds = 0;
    if (!voice_memo_time::epochToLocal(epochUtc_, &local) ||
        !voice_memo_time::localUtcOffsetSeconds(epochUtc_, &offsetSeconds)) {
        return;
    }

    Serial.printf("[time] Lisbon local: %04d-%02d-%02d %02d:%02d:%02d (%s, UTC%+d)\n",
                  local.year,
                  local.month,
                  local.day,
                  local.hour,
                  local.minute,
                  local.second,
                  local.dst ? "WEST" : "WET",
                  offsetSeconds / 3600);
}

}  // namespace voice_memo_ui

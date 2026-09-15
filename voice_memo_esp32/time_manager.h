#pragma once

// Single owner of the device clock.
//
// Responsibilities:
//   * install the Europe/Lisbon POSIX timezone (WET/WEST with the official EU
//     DST dates) for the process;
//   * read the PCF85063, which is treated strictly as a **UTC** source;
//   * when Wi-Fi is up, ask SNTP for the time (non-blocking) and write the
//     resulting UTC back into the PCF85063;
//   * convert RTC UTC -> local Europe/Lisbon and expose "HH:MM" to the UI.
//
// The UI never sees a timezone, an offset or an NTP detail: it only reads
// timeText(). When neither the RTC nor NTP can supply a trustworthy instant the
// text is "--:--" instead of a plausible-looking wrong clock.
//
// The local time is produced by time_zone.cpp (setenv("TZ") + tzset() +
// localtime_r()), so WET/WEST and both DST transitions are applied by the C
// library, never by a hard-coded +1 hour. The file `rtc_pcf85063.{h,cpp}` holds
// the chip, `time_zone.{h,cpp}` the pure conversion, and this class the policy
// that ties them together.

#include <Arduino.h>

#include "rtc_pcf85063.h"
#include "time_zone.h"

namespace voice_memo_ui {

class TimeManager {
public:
    explicit TimeManager(RtcPcf85063& rtc);

    // Applies the timezone and probes the RTC. Call once from setup().
    void begin();

    // Non-blocking; safe to call from every loop() iteration. `wifi_connected`
    // gates the NTP request, so the firmware runs purely from the RTC offline.
    void update(bool wifi_connected);

    // "HH:MM" in Europe/Lisbon, or "--:--" when no trustworthy time exists.
    const char* timeText() const { return text_; }

    // True when timeText() is a real local time.
    bool timeValid() const { return valid_; }
    // True when the PCF85063 answered the address probe.
    bool rtcAvailable() const { return rtc_.available(); }
    // True when the last RTC read produced a trustworthy UTC instant.
    bool rtcTimeValid() const { return rtcValid_; }
    // True once SNTP has delivered a real time in this session.
    bool ntpSynchronized() const { return ntpSynced_; }

private:
    void pollRtc();
    void serviceNtp(bool wifi_connected, uint32_t now);
    void syncRtcFromSystem(uint32_t now, int64_t systemEpoch);
    // Prints "[time] RTC UTC: ..." and "[time] Lisbon local: ..." once. Called
    // at boot and after every RTC correction, never per loop().
    void logClock();

    RtcPcf85063& rtc_;

    // Last published state.
    char text_[6] = {'-', '-', ':', '-', '-', '\0'};
    bool valid_ = false;
    int64_t epochUtc_ = 0;  // instant currently shown (UTC seconds)

    // RTC / NTP bookkeeping.
    bool rtcValid_ = false;
    int64_t rtcEpochUtc_ = 0;
    bool ntpSynced_ = false;
    bool ntpStarted_ = false;
    uint32_t ntpStartedMs_ = 0;
    bool rtcSyncedFromNtp_ = false;
    bool rtcFallbackLogged_ = false;
    bool loggedInvalid_ = false;

    // Deadline arithmetic (0 means "due now"), so nothing polls per loop().
    uint32_t nextRtcPollMs_ = 0;
    uint32_t nextRtcWriteMs_ = 0;
};

}  // namespace voice_memo_ui

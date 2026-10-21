#pragma once

// Pure scheduling helpers for the per-target safety guards (allowed hours and
// automatic stop). Kept free of OBS and Qt types so the logic can be unit
// tested on its own.

#include <optional>

struct ScheduleSpec {
    bool enabled = false;
    int startMin = 0;  // minutes since local midnight, 0..1439
    int endMin = 0;    // minutes since local midnight, 0..1439
    int days = 0x7F;   // bit 0 = Monday ... bit 6 = Sunday
};

// A moment in local time. dow: 0 = Monday ... 6 = Sunday.
struct LocalMoment {
    int dow = 0;
    int secondOfDay = 0; // 0..86399
};

inline bool DayAllowed(const ScheduleSpec& s, int dow)
{
    return (s.days >> (((dow % 7) + 7) % 7)) & 1;
}

// True when streaming is allowed at `now`. A disabled schedule always allows.
//  - start <  end: same-day window, e.g. 19:00-23:00
//  - start >  end: crosses midnight, e.g. 22:00-02:00; the day bit is the day
//                  the window STARTS on, so Fri 22:00-02:00 still runs at
//                  Sat 01:00 even if Saturday itself is not selected
//  - start == end: whole day, on the selected days
inline bool IsWithinWindow(const ScheduleSpec& s, const LocalMoment& now)
{
    if (!s.enabled)
        return true;

    const int start = s.startMin * 60;
    const int end = s.endMin * 60;
    const int t = now.secondOfDay;

    if (start == end)
        return DayAllowed(s, now.dow);
    if (start < end)
        return DayAllowed(s, now.dow) && t >= start && t < end;
    return (DayAllowed(s, now.dow) && t >= start)
        || (DayAllowed(s, now.dow + 6) && t < end);
}

// Seconds until the current window closes, or nullopt when there is no closing
// time (schedule disabled, whole-day window) or `now` is outside the window.
inline std::optional<int> SecondsUntilWindowEnd(const ScheduleSpec& s, const LocalMoment& now)
{
    if (!s.enabled || s.startMin == s.endMin || !IsWithinWindow(s, now))
        return std::nullopt;

    const int start = s.startMin * 60;
    const int end = s.endMin * 60;
    const int t = now.secondOfDay;

    if (start < end)
        return end - t;
    if (t >= start)
        return 24 * 3600 - t + end;
    return end - t;
}

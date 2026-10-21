#pragma once

// Per-target connection health, modelled on the coloured square in OBS's
// status bar. Pure functions so the thresholds can be unit tested.

#include <algorithm>
#include <cstdint>

enum class HealthLevel { Excellent, Good, Fair, Bad };

// Same smoothing OBS uses: average with the previous sample, but never below
// the current value (so spikes show immediately and fade over one tick), and
// clamped to 1.0.
inline float SmoothCongestion(float current, float previous)
{
    float avg = (current + previous) * 0.5f;
    if (avg < current)
        avg = current;
    return std::clamp(avg, 0.0f, 1.0f);
}

// congestion: 0.0 (none) .. 1.0 (saturated), as from obs_output_get_congestion.
// framesInInterval / droppedInInterval: counts over the last update interval,
// so the colour reflects how it's going now, not since the stream started.
inline HealthLevel ClassifyHealth(float congestion, int64_t framesInInterval, int64_t droppedInInterval)
{
    HealthLevel level;
    if (congestion < 0.10f)
        level = HealthLevel::Excellent;
    else if (congestion < 0.30f)
        level = HealthLevel::Good;
    else if (congestion < 0.70f)
        level = HealthLevel::Fair;
    else
        level = HealthLevel::Bad;

    // Dropped frames can only make the verdict worse, never better.
    const int64_t total = framesInInterval + droppedInInterval;
    if (droppedInInterval > 0 && total > 0) {
        const double pct = 100.0 * (double)droppedInInterval / (double)total;
        HealthLevel byDrops = pct >= 10.0 ? HealthLevel::Bad
                            : pct >= 1.0  ? HealthLevel::Fair
                                          : HealthLevel::Good;
        level = std::max(level, byDrops);
    }
    return level;
}

inline const char* HealthColor(HealthLevel level)
{
    switch (level) {
    case HealthLevel::Excellent: return "#2ecc71"; // green
    case HealthLevel::Good:      return "#a3d65c"; // yellow-green
    case HealthLevel::Fair:      return "#e6b800"; // yellow
    case HealthLevel::Bad:       return "#e74c3c"; // red
    }
    return "#888888";
}

inline const char* HealthTextKey(HealthLevel level)
{
    switch (level) {
    case HealthLevel::Excellent: return "Health.Excellent";
    case HealthLevel::Good:      return "Health.Good";
    case HealthLevel::Fair:      return "Health.Fair";
    case HealthLevel::Bad:       return "Health.Bad";
    }
    return "";
}

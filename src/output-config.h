#pragma once

#include <string>
#include <optional>
#include <vector>
#include <unordered_map>
#include <memory>
#include <list>

#include <json.hpp>

struct VideoEncoderConfig {
    std::string id;
    std::string encoderId;
    int fpsDenumerator = 1;
    nlohmann::json encoderParams;
    std::optional<std::string> outputScene;
    std::optional<std::string> resolution;
};
using VideoEncoderConfigPtr = std::shared_ptr<VideoEncoderConfig>;

struct AudioTrackConfig {
    int mixer_track;
    int output_track;
};
using AudioTrackConfigPtr = std::shared_ptr<AudioTrackConfig>;

struct AudioEncoderConfig {
    std::string id;
    std::string encoderId;
    nlohmann::json encoderParams;
    int mixerId = 0;
    std::list<AudioTrackConfigPtr> audioTracks;
};
using AudioEncoderConfigPtr = std::shared_ptr<AudioEncoderConfig>; 


struct OutputTargetConfig {
    std::string id;
    std::string name;
    std::string protocol = "RTMP";
    bool syncStart = false;
    bool syncStop = false;
    // When false, this target is skipped by "Start all" / sync-start and
    // StartStreaming() refuses to start it, without losing its saved
    // settings. Lets a target be temporarily excluded (e.g. a platform
    // that's down) without deleting and re-creating it.
    bool enabled = true;

    // --- Safety guards (all optional, off by default) ---
    // Ask "are you sure?" before this target starts, from any entry point
    // (button, Start all, hotkey, sync start).
    bool confirmStart = false;
    // Only allow this target to run inside a weekly time window. Times are
    // minutes since local midnight. A window with start > end crosses
    // midnight (e.g. 22:00-02:00); start == end means "all day".
    bool scheduleEnabled = false;
    int scheduleStartMin = 19 * 60;
    int scheduleEndMin = 23 * 60;
    // Bit 0 = Monday ... bit 6 = Sunday. For midnight-crossing windows the
    // bit refers to the day the window starts on.
    int scheduleDays = 0x7F;
    // Auto-stop after this many minutes of streaming (0 = no limit).
    int maxDurationMin = 0;
    // Show a warning this many minutes before an automatic stop (0 = none).
    // Capped at 25, below the 30 min "extend" step.
    int warnBeforeMin = 5;

    nlohmann::json serviceParam;
    nlohmann::json outputParam;

    std::optional<std::string> videoConfig;
    std::optional<std::string> audioConfig;
};
using OutputTargetConfigPtr = std::shared_ptr<OutputTargetConfig>;


struct MultiOutputConfig {
public:
    std::list<OutputTargetConfigPtr> targets;
    std::list<VideoEncoderConfigPtr> videoConfig;
    std::list<AudioEncoderConfigPtr> audioConfig;
};

template<class T, class S>
inline T FindById(std::list<T>& list, const S& id) {
    for(auto& x: list) {
        if (x->id == id)
            return x;
    }
    return nullptr;
}


MultiOutputConfig& GlobalMultiOutputConfig();

void SaveMultiOutputConfig();

bool LoadMultiOutputConfig();

std::string GenerateId(MultiOutputConfig& config);

// Used by the dock's Export/Import Config buttons to back up or restore
// all targets (including stream keys) outside of OBS's own profile
// folder, since the plugin otherwise only ever saves into the current
// profile's obs-multi-rtmp.json.
std::string SerializeMultiOutputConfig(MultiOutputConfig& config);

// Returns std::nullopt only if `content` isn't valid JSON; a validly
// parsed but unexpectedly-shaped file yields an (possibly empty) config
// instead of failing, same tolerance as the profile loader.
std::optional<MultiOutputConfig> DeserializeMultiOutputConfig(const std::string& content);

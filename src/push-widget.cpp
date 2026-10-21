#include "pch.h"
#include "helpers.h"
#include <regex>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <optional>
#include <tuple>
#include "push-widget.h"
#include "edit-widget.h"
#include "output-config.h"
#include "protocols.h"
#include "safety-guard.h"
#include "stream-health.h"

#include "obs.hpp"

class IOBSOutputEventHanlder
{
public:
    virtual void OnStarting() {}
    static void OnOutputStarting(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStarting();
    }

    virtual void OnStarted() {}
    static void OnOutputStarted(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStarted();
    }

    virtual void OnStopping() {}
    static void OnOutputStopping(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStopping();
    }
   
    virtual void OnStopped(int) {}
    static void OnOutputStopped(void* x, calldata_t* param)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnStopped(calldata_int(param, "code"));
    }

    virtual void OnReconnect() {}
    static void OnOutputReconnect(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnReconnect();
    }

    virtual void OnReconnected() {}
    static void OnOutputReconnected(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->OnReconnected();
    }

    virtual void onDeactive() {}
    static void OnOutputDeactive(void* x, calldata_t*)
    {
        auto thiz = static_cast<IOBSOutputEventHanlder*>(x);
        thiz->onDeactive();
    }

    void SetMeAsHandler(obs_output_t* output)
    {
        auto outputSignal = obs_output_get_signal_handler(output);
        if (outputSignal)
        {
            signal_handler_connect(outputSignal, "starting", &IOBSOutputEventHanlder::OnOutputStarting, this);
            signal_handler_connect(outputSignal, "start", &IOBSOutputEventHanlder::OnOutputStarted, this);
            signal_handler_connect(outputSignal, "reconnect", &IOBSOutputEventHanlder::OnOutputReconnect, this);
            signal_handler_connect(outputSignal, "reconnect_success", &IOBSOutputEventHanlder::OnOutputReconnected, this);
            signal_handler_connect(outputSignal, "stopping", &IOBSOutputEventHanlder::OnOutputStopping, this);
            signal_handler_connect(outputSignal, "deactivate", &IOBSOutputEventHanlder::OnOutputDeactive, this);
            signal_handler_connect(outputSignal, "stop", &IOBSOutputEventHanlder::OnOutputStopped, this);
        }
    }

    void DisconnectSignals(obs_output_t* output)
    {
        auto outputSignal = obs_output_get_signal_handler(output);
        if (outputSignal)
        {
            signal_handler_disconnect(outputSignal, "starting", &IOBSOutputEventHanlder::OnOutputStarting, this);
            signal_handler_disconnect(outputSignal, "start", &IOBSOutputEventHanlder::OnOutputStarted, this);
            signal_handler_disconnect(outputSignal, "reconnect", &IOBSOutputEventHanlder::OnOutputReconnect, this);
            signal_handler_disconnect(outputSignal, "reconnect_success", &IOBSOutputEventHanlder::OnOutputReconnected, this);
            signal_handler_disconnect(outputSignal, "stopping", &IOBSOutputEventHanlder::OnOutputStopping, this);
            signal_handler_disconnect(outputSignal, "deactivate", &IOBSOutputEventHanlder::OnOutputDeactive, this);
            signal_handler_disconnect(outputSignal, "stop", &IOBSOutputEventHanlder::OnOutputStopped, this);
        }
    }
};


class PushWidgetImpl : public PushWidget, public IOBSOutputEventHanlder
{
    std::string targetid_;
    OutputTargetConfigPtr config_;

    QPushButton* btn_ = 0;
    QLabel* name_ = 0;
    QLabel* msg_ = 0;
    QLabel* status_dot_ = 0;
    // Connection health while live (OBS-style coloured square + label).
    QLabel* health_ = 0;
    std::optional<HealthLevel> health_level_;
    float last_congestion_ = 0.0f;
    int64_t health_prev_frames_ = 0;
    int64_t health_prev_dropped_ = 0;
    QCheckBox* enabled_cb_ = 0;

    using clock = std::chrono::steady_clock;
    clock::time_point begin_time_;
    clock::time_point last_info_time_;
    uint64_t total_frames_ = 0;
    uint64_t total_bytes_ = 0;
    QTimer* timer_ = 0;

    // Safety guards: automatic stop and its advance warning.
    static constexpr int kExtendMinutes = 30;
    QTimer* limit_timer_ = 0;
    std::optional<clock::time_point> deadline_;
    bool warned_ = false;
    bool autoStopped_ = false;
    QPointer<QMessageBox> warnBox_;

    QPushButton* edit_btn_ = 0;
    QPushButton* remove_btn_ = 0;

    obs_output_t* output_ = 0;
    bool using_main_video_encoder_ = false;
    bool using_main_audio_encoder_ = false;
    obs_view_t* scene_view_ = 0;
    bool isUseDelay_ = false;
    std::atomic_bool isStarting_ = false;

    bool isConnecting_ = false;
    bool isReconnecting_ = false;
    int lastErrorCode_ = 0;

    // Registered under the stable target id (not the display name) so key
    // bindings survive renames. Their description label is set once at
    // registration time and won't update if the target is renamed later.
    obs_hotkey_id startHotkeyId_ = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id stopHotkeyId_ = OBS_INVALID_HOTKEY_ID;

    QPushButton* GetDeleteButton() {
        return remove_btn_;
    }

    bool PrepareOutputService()
    {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output service before output object is created.");
            return false;
        }
        
        ReleaseOutputService();
        
        auto conf = obs_data_create_from_json(config_->serviceParam.dump().c_str());

        auto protocolInfo = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
        assert(protocolInfo);
        if (!protocolInfo) {
        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
        	return false;
        }
        auto service_id = protocolInfo->serviceId;

        if (!conf)
            return false;
        
        auto service = obs_service_create(service_id, "multi-output-service", conf, nullptr);
        obs_data_release(conf);
        if (!service)
            return false;
        obs_output_set_service(output_, service);

        return true;
    }


    bool ReleaseOutputService()
    {
        if (!output_)
            return true;

        if (obs_output_active(output_)) {
            return false;
        }
        
        auto service = obs_output_get_service(output_);
        if (service)
        {
            obs_output_set_service(output_, nullptr);
            obs_service_release(service);
        }
        return true;
    }


    bool PrepareEncoderSource() {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output scene before output object is created.");
            return false;
        }

        if (!using_main_video_encoder_) {
            auto venc = obs_output_get_video_encoder(output_);
            if (!venc) {
                blog(LOG_ERROR, TAG "Prepare output scene before encoder is created.");
                return false;
            }
            auto videoConfig = FindById(GlobalMultiOutputConfig().videoConfig, config_->videoConfig.value_or(""));

            if (!videoConfig || !videoConfig->outputScene.has_value()) {
                obs_encoder_set_video(venc, obs_get_video());
            } else {
                auto sceneName = *videoConfig->outputScene;
                OBSSourceAutoRelease scene = obs_get_source_by_name(sceneName.c_str());
                if (scene == nullptr) {
                    blog(LOG_ERROR, TAG "Output scene is not found.");
                    return false;
                }
                ReleaseOutputSceneView();

                scene_view_ = obs_view_create();
                obs_view_set_source(scene_view_, 0, scene);
                obs_source_inc_active(scene);
                auto scene_video = obs_view_add(scene_view_);
                obs_encoder_set_video(venc, scene_video);
            }
        }

        if (!using_main_audio_encoder_) {
            auto aenc = obs_output_get_audio_encoder(output_, 0);
            if (!aenc) {
                blog(LOG_ERROR, TAG "Prepare output scene before encoder is created.");
                return false;
            }
            obs_encoder_set_audio(aenc, obs_get_audio());

            // Set other audio tracks
            auto audioConfig = FindById(GlobalMultiOutputConfig().audioConfig, config_->audioConfig.value_or(""));
            if (audioConfig) {
                for (auto& track : audioConfig->audioTracks) {
                    auto enc = obs_output_get_audio_encoder(output_, track->output_track);
                    if (enc) {
                        obs_encoder_set_audio(enc, obs_get_audio());
                    }
                }
            }
        }
        
        return true;
    }


    bool ReleaseOutputSceneView() {
        if (!scene_view_)
            return true;

        obs_view_remove(scene_view_);
        OBSSourceAutoRelease source = obs_view_get_source(scene_view_, 0);
        if (source) {
            obs_source_dec_active(source);
        }
        obs_view_set_source(scene_view_, 0, nullptr);
        obs_view_destroy(scene_view_);
        scene_view_ = nullptr;

        return true;
    }


    std::string VideoEncoderName() {
        return "multi-rtmp-venc" + config_->videoConfig.value_or("");
    }

    std::string AudioEncoderName(int track) {
        return "multi-rtmp-aenc" + config_->audioConfig.value_or("") + "-track-idx-" + std::to_string(track);
    }

    std::optional<std::tuple<int, int>> ParseResolution(const std::optional<std::string>& res) {
        if (!res.has_value())
            return std::nullopt;
        std::regex res_pattern(R"__(\s*(\d{1,5})\s*x\s*(\d{1,5})\s*)__");
        std::smatch match;
        if (std::regex_match(*res, match, res_pattern))
        {
            auto width = std::stoi(match[1].str());
            auto height = std::stoi(match[2].str());
            return {{ width, height }};
        }

        return std::nullopt;
    }

    OBSEncoder GetVideoEncoder() {
        auto config_id = config_->videoConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (config_id == "" || config_id == OBS_STREAMING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_streaming_output();
            OBSEncoder enc = obs_output_get_video_encoder(stream_output);
            using_main_video_encoder_ = true;
            return enc.Get();
        } else if (config_id == OBS_RECORDING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_recording_output();
            OBSEncoder enc = obs_output_get_video_encoder(stream_output);
            using_main_video_encoder_ = true;
            return enc.Get();
        } else {
            OBSEncoderAutoRelease enc = obs_get_encoder_by_name(VideoEncoderName().c_str());
            if (!enc) {
                auto& global = GlobalMultiOutputConfig();
                auto videoConfig = FindById(global.videoConfig, config_id);
                if (videoConfig) {
                    OBSDataAutoRelease settings = obs_data_create_from_json(videoConfig->encoderParams.dump().c_str());
                    enc = obs_video_encoder_create(videoConfig->encoderId.c_str(), VideoEncoderName().c_str(), settings, nullptr);
                    if (enc) {
                        auto wh = ParseResolution(videoConfig->resolution);
                        if (wh.has_value()) {
                            obs_encoder_set_gpu_scale_type(enc, obs_scale_type::OBS_SCALE_BICUBIC);
                            auto [w, h] = *wh;
                            obs_encoder_set_scaled_size(enc, w, h);
                        }
                        obs_encoder_set_frame_rate_divisor(enc, videoConfig->fpsDenumerator);
                    }
                } else {
                    assert(false && "No video encoder config found with specified id.");
                    blog(LOG_ERROR, TAG "Load video encoder config failed for %s. Sharing with main output.", config_->name.c_str());
                    config_->videoConfig = OBS_STREAMING_ENC_PLACEHOLDER;
                    return GetVideoEncoder();
                }
            }

            using_main_video_encoder_ = false;
            return enc.Get();
        }
    }

    OBSEncoder GetAudioEncoder(int trackIdx = 0, std::optional<int> mixerId = std::nullopt) {
        auto config_id = config_->audioConfig.value_or(OBS_STREAMING_ENC_PLACEHOLDER);
        if (config_id == "" || config_id == OBS_STREAMING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_streaming_output();
            OBSEncoder enc = obs_output_get_audio_encoder(stream_output, 0);
            using_main_audio_encoder_ = true;
            return enc.Get();
        } else if (config_id == OBS_RECORDING_ENC_PLACEHOLDER) {
            OBSOutputAutoRelease stream_output = obs_frontend_get_recording_output();
            OBSEncoder enc = obs_output_get_audio_encoder(stream_output, 0);
            using_main_audio_encoder_ = true;
            return enc.Get();
        } else {
            OBSEncoderAutoRelease enc = obs_get_encoder_by_name(AudioEncoderName(trackIdx).c_str());
            if (!enc) {
                auto& global = GlobalMultiOutputConfig();
                auto audioConfigId = *config_->audioConfig;
                auto audioConfig = FindById(global.audioConfig, audioConfigId);
                if (audioConfig) {
                    OBSDataAutoRelease settings = obs_data_create_from_json(audioConfig->encoderParams.dump().c_str());

                    // If we were provided with a mixerId, override the audioConfig's mixerId with it
                    int defaultMixerId = audioConfig->mixerId;
                    if (auto overrideMixerId = mixerId) {
                        defaultMixerId = *overrideMixerId;
                    }

                    enc = obs_audio_encoder_create(audioConfig->encoderId.c_str(), AudioEncoderName(trackIdx).c_str(), settings, defaultMixerId, nullptr);
                } else {
                    assert(false && "No audio encoder config found with specified id.");
                    blog(LOG_ERROR, TAG "Load audio encoder config failed for %s. Sharing with main output.", config_->name.c_str());
                    config_->audioConfig = OBS_STREAMING_ENC_PLACEHOLDER;
                    return GetAudioEncoder();
                }
            }
            
            using_main_audio_encoder_ = false;
            return enc.Get();
        }
    }

    bool PrepareOutputEncoders()
    {
        if (!output_) {
            blog(LOG_ERROR, TAG "Prepare output encoder before output object is created.");
            return false;
        }
        
        ReleaseOutputEncoder();

        auto& global = GlobalMultiOutputConfig();

        // main output
        OBSOutput mainOutput = obs_frontend_get_streaming_output();
        OBSOutput recordingOutput =  obs_frontend_get_recording_output();

        obs_output_release(mainOutput);
        obs_output_release(recordingOutput);

        OBSEncoder venc = GetVideoEncoder();
        OBSEncoder aenc = GetAudioEncoder();

        std::vector<std::tuple<int, OBSEncoder>> additionalTracks;
        if (auto audioConfigId = config_->audioConfig) {
            auto audioConfig = FindById(global.audioConfig, *audioConfigId);

            if (!audioConfig) {
                blog(LOG_ERROR, TAG "Load audio encoder config failed for %s. Could not determine additional tracks.", config_->name.c_str());
            } else {
                additionalTracks.reserve(audioConfig->audioTracks.size());
                for (auto& track : audioConfig->audioTracks) {
                    OBSEncoder enc = GetAudioEncoder(track->output_track, track->mixer_track);
                    if (enc) {
                        // Record the output track index and the encoder for later when we set the encoders on the output
                        additionalTracks.push_back({ track->output_track, enc });
                    }
                }
            }
        }

        if (!aenc || !venc) {
            // If we don't have a valid encoder, we're likely using a special encoder type that
            // needs to be started by the user (i.e. start streaming or start recording)
            ReleaseOutputEncoder();

            auto msgbox = new QMessageBox(QMessageBox::Icon::Critical, 
                obs_module_text("Notice.Title"), 
                obs_module_text("Notice.GetEncoder"),
                QMessageBox::StandardButton::Ok,
                this
                );
            msgbox->exec();
            return false;
        }

        obs_output_set_audio_encoder(output_, obs_encoder_get_ref(aenc), 0);
        for (auto& track : additionalTracks) {
            auto trackIdx = std::get<0>(track);
            auto enc = std::get<1>(track);
            obs_output_set_audio_encoder(output_, obs_encoder_get_ref(enc), trackIdx);
        }
        obs_output_set_video_encoder(output_, obs_encoder_get_ref(venc));

        return true;
    }


    bool ReleaseOutputEncoder()
    {
        if (!output_)
            return true;
        else if (obs_output_active(output_) == false)
        {
            auto venc = obs_output_get_video_encoder(output_);
            if (venc)
            {
                obs_output_set_video_encoder(output_, nullptr);
                obs_encoder_release(venc);
            }
            
            auto aenc = obs_output_get_audio_encoder(output_, 0);
            if (aenc)
            {
                obs_output_set_audio_encoder(output_, nullptr, 0);
                obs_encoder_release(aenc);
            }

            return true;
        }
        else {
            blog(LOG_ERROR, TAG "Release output while it is active.");
            return false;
        }
    }


    bool ReleaseOutput()
    {
        if (output_) {
            DisconnectSignals(output_);
        }

        if (output_ && obs_output_active(output_)) {
            obs_output_force_stop(output_);
        }

        if (output_ && obs_output_active(output_) == false)
        {
            bool ret = ReleaseOutputService();
            ret = ReleaseOutputEncoder() && ret;

            obs_output_release(output_);
            output_ = nullptr;

            ret = ReleaseOutputSceneView() && ret;

            return ret;
        }
        else if (output_) {
            obs_output_release(output_);
            output_ = nullptr;

            return true;
        }
        else if (output_ == nullptr)
            return true;
        else
            return false;
    }


    void UpdateStreamStatus() {
        using namespace std::chrono;

        if (!output_)
            return;

        static const char* units[] = {
            "bps", "Kbps", "Mbps", "Gbps", "Tbps", "Pbps", "Ebps", "Zbps", "Ybps"
        };

        auto new_bytes = obs_output_get_total_bytes(output_);
        auto new_frames = obs_output_get_total_frames(output_);
        auto now = clock::now();

        auto interval = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_info_time_).count();
        if (interval > 0)
        {
            auto duration = now - begin_time_;
            auto hh = duration_cast<hours>(duration);
            duration -= hh;
            auto mm = duration_cast<minutes>(duration);
            duration -= mm;
            auto ss = duration_cast<seconds>(duration);
            duration -= ss;

            char strDuration[64] = { 0 };
            snprintf(strDuration, sizeof(strDuration), "%02d:%02d:%02d", (int)hh.count(), (int)mm.count(), (int)ss.count());

            char strFps[32] = { 0 };
            snprintf(strFps, sizeof(strFps), "%d FPS", static_cast<int>(std::round((new_frames - total_frames_) / interval)));

            auto bps = (new_bytes - total_bytes_) * 8 / interval;
            auto strBps = [&]()-> std::string {
                if (bps > 0)
                {
                    int unitMaxIndex = sizeof(units) / sizeof(*units);
                    int unitIndex = static_cast<int>(log10(bps) / 3);
                    if (unitIndex >= unitMaxIndex)
                        unitIndex = unitMaxIndex - 1;
                    auto strVal = std::to_string(bps / pow(1000, unitIndex)).substr(0, 4);
                    if (!strVal.empty() && strVal.back() == '.')
                        strVal.pop_back();
                    return strVal + " " + units[unitIndex];
                }
                else
                {
                    return "0 bps";
                }
            }();
            
            std::string line = std::string(strDuration) + "  " + strBps + "  " + strFps;

            // Health: congestion (smoothed like OBS) plus frames dropped in
            // this interval. Counters can restart after a reconnect, so
            // negative deltas are treated as zero.
            {
                const int64_t frames = obs_output_get_total_frames(output_);
                const int64_t dropped = obs_output_get_frames_dropped(output_);
                const int64_t dFrames = std::max<int64_t>(0, frames - health_prev_frames_);
                const int64_t dDropped = std::max<int64_t>(0, dropped - health_prev_dropped_);
                health_prev_frames_ = frames;
                health_prev_dropped_ = dropped;

                const float rawCongestion = obs_output_get_congestion(output_);
                const float congestion = SmoothCongestion(rawCongestion, last_congestion_);
                last_congestion_ = rawCongestion;
                health_level_ = ClassifyHealth(congestion, dFrames, dDropped);

                // Cumulative dropped frames, as in OBS's status bar.
                if (frames + dropped > 0) {
                    const double pct = 100.0 * (double)dropped / (double)(frames + dropped);
                    auto droppedText = QString::fromUtf8(obs_module_text("Health.Dropped"))
                        .arg((qlonglong)dropped)
                        .arg(QString::number(pct, 'f', 1));
                    line += "  ";
                    line += droppedText.toUtf8().constData();
                }

                if (status_dot_)
                    status_dot_->setToolTip(QString::fromUtf8(obs_module_text("Health.Congestion"))
                        .arg(QString::number(congestion * 100.0, 'f', 0)));
                UpdateStatusDot();
            }

            // Show how long until the automatic stop, if one is armed.
            if (deadline_) {
                auto left = duration_cast<seconds>(*deadline_ - now).count();
                if (left < 0) left = 0;
                char strLeft[64] = { 0 };
                snprintf(strLeft, sizeof(strLeft), "  [-%02d:%02d:%02d]",
                    (int)(left / 3600), (int)((left % 3600) / 60), (int)(left % 60));
                line += strLeft;
            }

            msg_->setText(QString::fromUtf8(line.c_str()));
        }

        total_frames_ = new_frames;
        total_bytes_ = new_bytes;
        last_info_time_ = now;
    }

    void UpdateStatusDot() {
        if (!status_dot_)
            return;

        const char* color = "#888888"; // stopped / idle
        QString label;
        if (isReconnecting_) {
            color = "#e67e22"; // orange: link lost, retrying
            label = QString::fromUtf8(obs_module_text("Status.Reconnecting"));
        } else if (isConnecting_) {
            color = "#e6b800";
            label = QString::fromUtf8(obs_module_text("Status.Connecting"));
        } else if (IsRunning()) {
            if (health_level_) {
                color = HealthColor(*health_level_);
                label = QString::fromUtf8(obs_module_text(HealthTextKey(*health_level_)));
            } else {
                color = "#2ecc71"; // live, no sample yet
                label = QString::fromUtf8(obs_module_text("Status.Streaming"));
            }
        } else if (lastErrorCode_ != 0) {
            color = "#e74c3c"; // last attempt failed
            label = QString::fromUtf8(obs_module_text("Health.Failed"));
        }

        status_dot_->setStyleSheet(QString("color: %1;").arg(color));
        if (!IsRunning())
            status_dot_->setToolTip(QString());
        if (health_) {
            health_->setText(label);
            health_->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color));
        }
    }

    // ---- Safety guards -------------------------------------------------

    ScheduleSpec GetSchedule() const
    {
        ScheduleSpec s;
        s.enabled = config_->scheduleEnabled;
        s.startMin = config_->scheduleStartMin;
        s.endMin = config_->scheduleEndMin;
        s.days = config_->scheduleDays;
        return s;
    }

    static LocalMoment NowLocal()
    {
        const auto dt = QDateTime::currentDateTime();
        LocalMoment m;
        m.dow = dt.date().dayOfWeek() - 1; // Qt: 1 = Monday ... 7 = Sunday
        m.secondOfDay = QTime(0, 0).secsTo(dt.time());
        return m;
    }

    static QString FormatMinutes(int minutes)
    {
        return QString("%1:%2")
            .arg(minutes / 60, 2, 10, QChar('0'))
            .arg(minutes % 60, 2, 10, QChar('0'));
    }

    // Fixed once per run (at start) from the max duration and/or the end of
    // the allowed window, whichever comes first. Extending moves it later.
    // It is not recomputed each tick so that an extension can run past the
    // window end without the limit silently disappearing.
    std::optional<clock::time_point> ComputeDeadline() const
    {
        std::optional<clock::time_point> deadline;
        if (config_->maxDurationMin > 0)
            deadline = begin_time_ + std::chrono::minutes(config_->maxDurationMin);

        std::optional<clock::time_point> windowEnd;
        const auto schedule = GetSchedule();
        const auto now = NowLocal();
        if (auto secs = SecondsUntilWindowEnd(schedule, now)) {
            windowEnd = clock::now() + std::chrono::seconds(*secs);
        } else if (!IsWithinWindow(schedule, now)) {
            // Already outside the allowed hours (the window closed while the
            // output was connecting, or the schedule was edited mid-stream).
            // Without this the run would have no limit at all. Give the
            // usual warning period instead of cutting instantly.
            windowEnd = clock::now() + std::chrono::minutes(config_->warnBeforeMin);
        }

        if (windowEnd && (!deadline || *windowEnd < *deadline))
            deadline = windowEnd;
        return deadline;
    }

    bool ConfirmStart()
    {
        auto text = QString::fromUtf8(obs_module_text("Question.Start"))
            .arg(QString::fromUtf8(config_->name));
        auto res = QMessageBox::question(
            this,
            QString::fromUtf8(obs_module_text("Question.Title")),
            text,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No // default answer: No, so a stray Enter doesn't start it
        );
        return res == QMessageBox::Yes;
    }

    void StopGracefully()
    {
        if (output_ && obs_output_active(output_))
            obs_output_stop(output_);
    }

    void AutoStop()
    {
        autoStopped_ = true;
        deadline_.reset(); // don't fire again while the output winds down
        if (warnBox_)
            warnBox_->close();
        blog(LOG_INFO, TAG "Auto-stopping target \"%s\" (time limit reached)", config_->name.c_str());
        StopGracefully();
    }

    void ShowStopWarning()
    {
        if (warnBox_)
            return;

        using namespace std::chrono;
        auto remaining = duration_cast<seconds>(*deadline_ - clock::now()).count();
        auto mins = std::max<long long>(1, (remaining + 59) / 60);

        auto text = QString::fromUtf8(obs_module_text("Warn.AutoStop"))
            .arg(QString::fromUtf8(config_->name))
            .arg(mins);

        // Non-modal on purpose: it must not freeze OBS, and if nobody answers
        // the stop still happens at the deadline.
        auto box = new QMessageBox(
            QMessageBox::Warning,
            QString::fromUtf8(obs_module_text("Notice.Title")),
            text,
            QMessageBox::NoButton,
            this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowModality(Qt::NonModal);
        auto extendBtn = box->addButton(
            QString::fromUtf8(obs_module_text("Warn.Extend")).arg(kExtendMinutes),
            QMessageBox::AcceptRole);
        auto stopBtn = box->addButton(
            QString::fromUtf8(obs_module_text("Warn.StopNow")),
            QMessageBox::DestructiveRole);
        box->setDefaultButton(extendBtn);
        warnBox_ = box;

        QObject::connect(box, &QMessageBox::buttonClicked, this,
            [this, extendBtn, stopBtn](QAbstractButton* clicked) {
                if (clicked == extendBtn) {
                    if (deadline_) {
                        *deadline_ += std::chrono::minutes(kExtendMinutes);
                        warned_ = false; // warn again before the new deadline
                    }
                } else if (clicked == stopBtn) {
                    deadline_.reset();
                    StopGracefully();
                }
            });

        box->show();
        box->raise();
        QApplication::alert(box);
    }

    void CheckLimits()
    {
        if (!deadline_ || !IsRunning())
            return;

        const auto now = clock::now();
        if (now >= *deadline_) {
            AutoStop();
            return;
        }

        if (!warned_ && config_->warnBeforeMin > 0
            && *deadline_ - now <= std::chrono::minutes(config_->warnBeforeMin)) {
            warned_ = true;
            ShowStopWarning();
        }
    }

public:
    PushWidgetImpl(const std::string& targetid, QWidget* parent = 0)
        : QWidget(parent)
        , targetid_(targetid)
    {
        QObject::setObjectName("push-widget");

        auto& global = GlobalMultiOutputConfig();
        config_ = FindById(global.targets, targetid_);
        if (!config_)
            return;

        timer_ = new QTimer(this);
        timer_->setInterval(std::chrono::milliseconds(1000));
        QObject::connect(timer_, &QTimer::timeout, [this]() {
            UpdateStreamStatus();
        });

        // Runs for the whole life of a run, including reconnects (unlike
        // timer_, which pauses while reconnecting).
        limit_timer_ = new QTimer(this);
        limit_timer_->setInterval(std::chrono::milliseconds(1000));
        QObject::connect(limit_timer_, &QTimer::timeout, [this]() {
            CheckLimits();
        });

        auto layout = new QGridLayout(this);
        layout->addWidget(status_dot_ = new QLabel(u8"■", this), 0, 0);
        layout->addWidget(name_ = new QLabel(obs_module_text("NewStreaming"), this), 0, 1, 1, 2);
        layout->addWidget(health_ = new QLabel(this), 0, 3);
        health_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        layout->addWidget(enabled_cb_ = new QCheckBox(this), 1, 0);
        enabled_cb_->setToolTip(obs_module_text("Target.Enabled"));
        QObject::connect(enabled_cb_, &QCheckBox::clicked, [this](bool checked) {
            SetEnabled(checked);
        });

        layout->addWidget(btn_ = new QPushButton(obs_module_text("Btn.Start"), this), 1, 1);
        QObject::connect(btn_, &QPushButton::clicked, [this]() {
            StartStop();
        });

        layout->addWidget(edit_btn_ = new QPushButton(obs_module_text("Btn.Edit"), this), 1, 2);
        QObject::connect(edit_btn_, &QPushButton::clicked, [this]() {
            ShowEditDlg();
        });

        layout->addWidget(remove_btn_ = new QPushButton(obs_module_text("Btn.Delete"), this), 1, 3);

        // Keep action buttons usable in narrow docks across OBS themes.
        for (auto button : { btn_, edit_btn_, remove_btn_ }) {
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            button->setMinimumWidth(0);
            button->setMinimumHeight(button->fontMetrics().height() + 8);
        }
        layout->setColumnStretch(1, 1);
        layout->setColumnStretch(2, 1);
        layout->setColumnStretch(3, 1);

        layout->addWidget(msg_ = new QLabel(u8"", this), 2, 0, 1, 4);
        msg_->setWordWrap(true);
        layout->addItem(new QSpacerItem(0, 10), 3, 0);
        setLayout(layout);

        LoadConfig();
        UpdateStatusDot();

        auto startDesc = std::string(obs_module_text("Hotkey.StartTarget")) + " - " + config_->name;
        startHotkeyId_ = obs_hotkey_register_frontend(
            ("obs-multi-rtmp.start." + targetid_).c_str(),
            startDesc.c_str(),
            [](void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
                if (!pressed)
                    return;
                static_cast<PushWidgetImpl*>(data)->StartStreaming();
            },
            this
        );

        auto stopDesc = std::string(obs_module_text("Hotkey.StopTarget")) + " - " + config_->name;
        stopHotkeyId_ = obs_hotkey_register_frontend(
            ("obs-multi-rtmp.stop." + targetid_).c_str(),
            stopDesc.c_str(),
            [](void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
                if (!pressed)
                    return;
                static_cast<PushWidgetImpl*>(data)->StopStreaming();
            },
            this
        );
    }

    ~PushWidgetImpl()
    {
        if (startHotkeyId_ != OBS_INVALID_HOTKEY_ID)
            obs_hotkey_unregister(startHotkeyId_);
        if (stopHotkeyId_ != OBS_INVALID_HOTKEY_ID)
            obs_hotkey_unregister(stopHotkeyId_);

        ReleaseOutput();
    }


    void StartStreaming() override {
        // Hotkeys fire on OBS's hotkey thread. The checks below show dialogs
        // and touch widgets, so hop over to the UI thread first.
        // Using `this` as the context object makes Qt drop the call if the
        // widget is deleted before it runs.
        if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
            QMetaObject::invokeMethod(this, [this]() { StartStreaming(); }, Qt::QueuedConnection);
            return;
        }

        bool expected = false;
        if (IsRunning() || !isStarting_.compare_exchange_strong(expected, true))
            return;

        auto resetStarting = finally([this] { isStarting_ = false; });

        if (!config_->enabled) {
            SetMsg(obs_module_text("Status.Disabled"));
            return;
        }

        // Safety guards. Every way of starting a target ends up here (button,
        // Start all, hotkeys, sync start), so checking here covers them all.
        // The schedule is checked before asking for confirmation so the user
        // isn't asked to approve something that will be refused anyway.
        if (!IsWithinWindow(GetSchedule(), NowLocal())) {
            SetMsg(QString::fromUtf8(obs_module_text("Status.OutsideSchedule"))
                .arg(FormatMinutes(config_->scheduleStartMin))
                .arg(FormatMinutes(config_->scheduleEndMin)));
            return;
        }

        if (config_->confirmStart) {
            if (!ConfirmStart())
                return;
            // The dialog may have been left open past the end of the window.
            if (!IsWithinWindow(GetSchedule(), NowLocal())) {
                SetMsg(QString::fromUtf8(obs_module_text("Status.OutsideSchedule"))
                    .arg(FormatMinutes(config_->scheduleStartMin))
                    .arg(FormatMinutes(config_->scheduleEndMin)));
                return;
            }
        }

        // recreate output
        ReleaseOutput();

        if (output_ == nullptr)
        {
            obs_data* output_settings = obs_data_create_from_json(config_->outputParam.dump().c_str());

            auto protocolInfo = GetProtocolInfos()->GetInfo(config_->protocol.c_str());
            assert(protocolInfo);
            if (!protocolInfo) {
	        	blog(LOG_ERROR, TAG "Invalid protocol \"%s\", maybe broken config file.", config_->protocol.c_str());
	        	protocolInfo = GetProtocolInfos()->GetList();
	        }
            auto output_id = protocolInfo->outputId;

            blog(LOG_DEBUG, "Streaming to output: %s", output_id);

            output_ = obs_output_create(output_id, "multi-output", output_settings, nullptr);
            SetMeAsHandler(output_);
        }

        if (output_) {
            isUseDelay_ = false;

            auto profileConfig = obs_frontend_get_profile_config();
            if (profileConfig) {
                bool useDelay = config_get_bool(profileConfig, "Output", "DelayEnable");
                bool preserveDelay = config_get_bool(profileConfig, "Output", "DelayPreserve");
                int delaySec = config_get_int(profileConfig, "Output", "DelaySec");
                obs_output_set_delay(output_,
                    useDelay ? delaySec : 0,
                    preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0
                );

                if (useDelay && delaySec > 0)
                    isUseDelay_ = true;
            }
        }

        if (!PrepareOutputService())
        {
            SetMsg(obs_module_text("Error.CreateRtmpService"));
            return;
        }

        if (!PrepareOutputEncoders())
        {
            SetMsg(obs_module_text("Error.CreateEncoder"));
            return;
        }

        if (!PrepareEncoderSource())
        {
            SetMsg(obs_module_text("Error.SceneNotExist"));
            return;
        }

        if (!obs_output_start(output_))
        {
            SetMsg(obs_module_text("Error.StartOutput"));
            return;
        }

        // Start is asynchronous: keep the flag set until OnStarted()/OnStopped() clears it.
        resetStarting.Dismiss();
    }

    void StopStreaming() override {
        // Same as StartStreaming: the stop hotkey runs on the hotkey thread,
        // and the "discard delay?" question below is a dialog.
        if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
            QMetaObject::invokeMethod(this, [this]() { StopStreaming(); }, Qt::QueuedConnection);
            return;
        }

        if (!IsRunning())
            return;
        
        bool useForce = false;
        if (isUseDelay_) {
            auto res = QMessageBox(QMessageBox::Icon::Information,
                "?",
                obs_module_text("Ques.DropDelay"),
                QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No,
                this
            ).exec();
            if (res == QMessageBox::Yes)
                useForce = true;
        }

        if (!useForce)
            obs_output_stop(output_);
        else
            obs_output_force_stop(output_);
    }
   
    void OnOBSEvent(obs_frontend_event ev) override
    {
        if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_EXIT
            || ev == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_CHANGED
            || ev == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED
        ) {
            Stop();
        } else if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_STREAMING_STARTING) {
            if (!IsRunning() && config_->syncStart) {
                StartStop();
            }
        } else if (ev == obs_frontend_event::OBS_FRONTEND_EVENT_STREAMING_STOPPING) {
            if (IsRunning() && config_->syncStop) {
                StartStop();
            }
        }
    }

    void LoadConfig()
    {
        name_->setText(QString::fromUtf8(config_->name));
        if (enabled_cb_)
            enabled_cb_->setChecked(config_->enabled);
    }

    void ResetInfo()
    {
        total_frames_ = 0;
        total_bytes_ = 0;
        last_info_time_ = clock::now();
        msg_->setText("");

        health_level_.reset();
        last_congestion_ = 0.0f;
        health_prev_frames_ = output_ ? obs_output_get_total_frames(output_) : 0;
        health_prev_dropped_ = output_ ? obs_output_get_frames_dropped(output_) : 0;
    }

    bool IsRunning() override
    {
        return output_ != nullptr && obs_output_active(output_);
    }

    bool GetEnabled() override { return config_->enabled; }

    void SetEnabled(bool enabled) override
    {
        config_->enabled = enabled;
        if (enabled_cb_ && enabled_cb_->isChecked() != enabled)
            enabled_cb_->setChecked(enabled);
        if (!enabled && IsRunning())
            obs_output_force_stop(output_);
        SaveMultiOutputConfig();
        UpdateStatusDot();
    }

    void StartStop()
    {
        if (IsRunning())
        {
            StopStreaming();
            return;
        }

        StartStreaming();
    }

    void Stop()
    {
        if (IsRunning())
        {
            obs_output_force_stop(output_);
        }
    }

    bool ShowEditDlg() override
    {
        std::unique_ptr<EditOutputWidget> dlg{ createEditOutputWidget(targetid_, (QMainWindow*)obs_frontend_get_main_window()) };

        if (dlg->exec() == QDialog::DialogCode::Accepted)
        {
            SaveMultiOutputConfig();
            LoadConfig();
            // Limits edited while live take effect right away instead of
            // on the next start.
            if (IsRunning()) {
                deadline_ = ComputeDeadline();
                warned_ = false;
            }
            return true;
        }
        else
            return false;
    }

    void SetMsg(QString msg)
    {
        msg_->setText(msg);
        msg_->setToolTip(msg);
    }

    // obs logical
    void OnStarting() override
    {
        GetGlobalService().RunInUIThread([this]() {
            begin_time_ = clock::now();
            isConnecting_ = true;
            isReconnecting_ = false;
            lastErrorCode_ = 0;

            autoStopped_ = false;
            warned_ = false;
            deadline_ = ComputeDeadline();
            limit_timer_->start();

            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Connecting"));
            remove_btn_->setEnabled(false);
            UpdateStatusDot();
        });
    }

    void OnStarted() override
    {
        GetGlobalService().RunInUIThread([this]() {
            isStarting_ = false;
            isConnecting_ = false;
            isReconnecting_ = false;
            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Streaming"));

            ResetInfo();
            timer_->start();
            UpdateStatusDot();
        });
    }

    void OnReconnect() override
    {
        GetGlobalService().RunInUIThread([this]() {
            timer_->stop();
            isReconnecting_ = true;

            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Reconnecting"));
            UpdateStatusDot();
        });
    }

    void OnReconnected() override
    {
        GetGlobalService().RunInUIThread([this]() {
            isReconnecting_ = false;
            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Streaming"));

            ResetInfo();
            timer_->start();
            UpdateStatusDot();
        });
    }

    void OnStopping() override
    {
        GetGlobalService().RunInUIThread([this]() {
            timer_->stop();

            remove_btn_->setEnabled(false);
            btn_->setText(obs_module_text("Status.Stop"));
            btn_->setEnabled(true);
            SetMsg(obs_module_text("Status.Stopping"));
        });
    }

    void OnStopped(int code) override
    {
        GetGlobalService().RunInUIThread([this, code]() {
            isStarting_ = false;
            isConnecting_ = false;
            isReconnecting_ = false;
            lastErrorCode_ = code;
            ResetInfo();
            timer_->stop();

            limit_timer_->stop();
            deadline_.reset();
            if (warnBox_)
                warnBox_->close();

            remove_btn_->setEnabled(true);
            btn_->setText(obs_module_text("Btn.Start"));
            btn_->setEnabled(true);
            SetMsg(u8"");

            switch(code)
            {
                case 0:
                    SetMsg(autoStopped_ ? obs_module_text("Status.AutoStopped") : u8"");
                    autoStopped_ = false;
                    break;
                case -1:
                    SetMsg(obs_module_text("Error.WrongRTMPUrl"));
                    break;
                case -2:
                    SetMsg(obs_module_text("Error.ServerConnect"));
                    break;
                case -3:
                    SetMsg(obs_module_text("Error.ServerHandshake"));
                    break;
                case -4:
                    SetMsg(obs_module_text("Error.ServerRefuse"));
                    break;
                default:
                    SetMsg(obs_module_text("Error.Unknown"));
                    break;
            }

            UpdateStatusDot();
        });

        ReleaseOutputEncoder();
        ReleaseOutputSceneView();
    }
};

PushWidget* createPushWidget(const std::string& targetid, QWidget* parent) {
    return new PushWidgetImpl(targetid, parent);
}

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "station.h"
#include "station_manager.h"

namespace radio {

enum class RadioState {
    kStopped,
    kConnecting,
    kBuffering,
    kPlaying,
    kError
};

struct StreamMetadata {
    std::string title;         // StreamTitle from ICY metadata
    std::string artist;
    std::string album;
    int bitrate = 0;           // current bitrate if available
    int sample_rate = 0;       // decoder output sample rate
    int channels = 0;          // decoder output channels
    std::string codec;         // mp3, aac, flac, etc.
};

// Abstract interface for radio source. The single concrete implementation
// (RadioSourceHLS, built on esp_audio_simple_player + esp_hls_io) lives in
// radio_source_esp_adf.cc, included by radio_factory.cc.
class RadioSource {
public:
    using PcmCallback = std::function<void(std::vector<int16_t>&& pcm, int sample_rate, int channels)>;
    using MetadataCallback = std::function<void(const StreamMetadata& meta)>;
    using StateCallback = std::function<void(RadioState state, const std::string& message)>;

    virtual ~RadioSource() = default;

    // Start playing from URL
    virtual bool Start(const std::string& url) = 0;

    // Stop playback
    virtual void Stop() = 0;

    // Pause/resume (if supported by stream)
    virtual void Pause() = 0;
    virtual void Resume() = 0;

    // Volume control (0-100)
    virtual void SetVolume(int volume) = 0;
    virtual int GetVolume() const = 0;

    // Current state
    virtual RadioState GetState() const = 0;
    virtual bool IsPlaying() const = 0;
    virtual std::string GetCurrentUrl() const = 0;

    // Callbacks
    virtual void SetPcmCallback(PcmCallback cb) = 0;
    virtual void SetMetadataCallback(MetadataCallback cb) = 0;
    virtual void SetStateCallback(StateCallback cb) = 0;

    // Station management
    virtual bool SetStation(const StationInfo& station) = 0;
    virtual StationInfo GetCurrentStation() const = 0;

    // Station manager access
    virtual StationManager& GetStationManager() = 0;

    // Playback mode
    enum class PlaybackMode {
        kRadio,
        kLocal
    };
    virtual void SetPlaybackMode(PlaybackMode mode) = 0;
};

}  // namespace radio

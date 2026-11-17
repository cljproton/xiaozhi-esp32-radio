#pragma once

#include <vector>
#include <string>
#include <optional>
#include <mutex>
#include <nvs.h>
#include <nvs_flash.h>
#include "station.h"

namespace radio {

class StationManager {
public:
    struct Station {
        std::string id;          // unique identifier
        std::string name;
        std::string url;
        std::string genre;
        std::string country;
        int bitrate = 0;
        bool favorite = false;
        int play_count = 0;
        int64_t last_played = 0; // timestamp
        int gain = 100;          // playback gain percentage (100 = normal)
    };

    StationManager();
    ~StationManager();

    // NVS holds 48K; worst-case station JSON (~450 B) takes ~16 NVS slots,
    // and one page must stay spare for compaction. 80 fits with margin.
    static constexpr size_t kMaxStations = 80;

    // Initialize NVS storage. The list starts empty; stations are managed
    // from the device's web page (see radio_web_server.cc).
    bool Init(const char* nvs_namespace = "radio_stations");
    
    // Get all stations
    std::vector<Station> GetAllStations() const;
    
    // Get next station in list (circular)
    std::optional<Station> GetNextStation() const;
    
    // Get previous station in list (circular)
    std::optional<Station> GetPrevStation() const;

    // Move station by 1-based list position; dir is "up", "down", or "top".
    bool MoveStation(int index, const char* dir);

    // Find station by URL
    std::optional<Station> FindByUrl(const std::string& url) const;

    // Find stations by name (case-insensitive). An exact match returns one
    // entry; otherwise every substring match is returned.
    std::vector<Station> FindByName(const std::string& name) const;

    // Add/update station
    bool AddStation(const Station& station);
    bool UpdateStation(const Station& station);
    
    // Remove station
    bool RemoveStation(const std::string& id);
    
    // Record play (update play count and last_played). Accepts a station id
    // or its stream URL.
    void RecordPlay(const std::string& id_or_url);

private:
    std::string nvs_namespace_;
    mutable std::mutex mutex_;
    std::vector<Station> stations_;
    bool initialized_ = false;
    
    // NVS keys
    static constexpr const char* kKeyCount = "count";
    static constexpr const char* kKeyPrefix = "station_";
    
    // Save single station to NVS
    bool SaveStationToNvs(const Station& station, int index);
    
    // Load all stations from NVS
    bool LoadStationsFromNvs();
    
    // Save all stations to NVS
    bool SaveAllStationsToNvs();
    
    // Generate unique ID
    std::string GenerateId() const;
};

}  // namespace radio
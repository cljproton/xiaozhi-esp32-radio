#include "station_manager.h"
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <ctime>

namespace radio {

static const char* TAG = "StationManager";

StationManager::StationManager() = default;

StationManager::~StationManager() {
    if (initialized_) {
        SaveAllStationsToNvs();
    }
}

bool StationManager::Init(const char* nvs_namespace) {
    nvs_namespace_ = nvs_namespace;
    
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    
    // One-time migration from the preset era: preset stations were seeded
    // under a defaults_ver marker. The list is web-managed now, so wipe the
    // presets once and never write that marker again.
    nvs_handle_t handle;
    err = nvs_open(nvs_namespace_.c_str(), NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        uint32_t marker = 0;
        if (nvs_get_u32(handle, "defaults_ver", &marker) == ESP_OK) {
            ESP_LOGI(TAG, "Preset stations found, wiping (list is web-managed now)");
            nvs_iterator_t it = nullptr;
            esp_err_t it_err = nvs_entry_find("nvs", nvs_namespace_.c_str(), NVS_TYPE_ANY, &it);
            while (it_err == ESP_OK) {
                nvs_entry_info_t info;
                nvs_entry_info(it, &info);
                if (strncmp(info.key, kKeyPrefix, strlen(kKeyPrefix)) == 0 ||
                    strcmp(info.key, kKeyCount) == 0) {
                    nvs_erase_key(handle, info.key);
                }
                it_err = nvs_entry_next(&it);
            }
            nvs_release_iterator(it);
            nvs_erase_key(handle, "defaults_ver");
            nvs_commit(handle);
        }
        nvs_close(handle);
    }

    if (!LoadStationsFromNvs()) {
        ESP_LOGI(TAG, "No stations saved yet; open the device web page to add some");
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "Station manager initialized with %zu stations", stations_.size());
    return true;
}

std::vector<StationManager::Station> StationManager::GetAllStations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stations_;
}

std::optional<StationManager::Station> StationManager::FindByUrl(const std::string& url) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : stations_) {
        if (s.url == url) return s;
    }
    return std::nullopt;
}

std::vector<StationManager::Station> StationManager::FindByName(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::string query = lower(name);
    std::vector<Station> matches;

    for (const auto& s : stations_) {
        if (lower(s.name) == query) {
            return {s};
        }
        if (lower(s.name).find(query) != std::string::npos) {
            matches.push_back(s);
        }
    }
    return matches;
}

bool StationManager::AddStation(const Station& station) {    std::lock_guard<std::mutex> lock(mutex_);

    if (stations_.size() >= kMaxStations) {
        ESP_LOGW(TAG, "Station list full (%zu max)", kMaxStations);
        return false;
    }

    // Check for duplicate URL
    for (const auto& s : stations_) {
        if (s.url == station.url) {
            ESP_LOGW(TAG, "Station with URL already exists");
            return false;
        }
    }
    
    Station new_station = station;
    if (new_station.id.empty()) new_station.id = GenerateId();
    
    stations_.push_back(new_station);
    return SaveAllStationsToNvs();
}

bool StationManager::UpdateStation(const Station& station) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& s : stations_) {
        if (s.id == station.id) {
            s = station;
            return SaveAllStationsToNvs();
        }
    }
    return false;
}

bool StationManager::RemoveStation(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::remove_if(stations_.begin(), stations_.end(),
                             [&id](const Station& s) { return s.id == id; });
    if (it != stations_.end()) {
        stations_.erase(it, stations_.end());
        return SaveAllStationsToNvs();
    }
    return false;
}

bool StationManager::MoveStation(int index, const char* dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 1 || index > static_cast<int>(stations_.size())) {
        return false;
    }
    int from = index - 1;
    int to = -1;
    if (strcmp(dir, "top") == 0) {
        to = 0;
    } else if (strcmp(dir, "up") == 0) {
        to = from - 1;
    } else if (strcmp(dir, "down") == 0) {
        to = from + 1;
    }
    if (to < 0 || to >= static_cast<int>(stations_.size())) {
        // Boundary no-op (top station asked to move up / last asked to move down) is not an error.
        return true;
    }
    std::swap(stations_[from], stations_[to]);
    return SaveAllStationsToNvs();
}

void StationManager::RecordPlay(const std::string& id_or_url) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& s : stations_) {
        if (s.id == id_or_url || s.url == id_or_url) {
            s.play_count++;
            s.last_played = time(nullptr);
            SaveAllStationsToNvs();  // Async save would be better
            break;
        }
    }
}

bool StationManager::SaveStationToNvs(const Station& station, int index) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    
    char key[32];
    snprintf(key, sizeof(key), "%s%d", kKeyPrefix, index);
    
    // Serialize to JSON string
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", station.id.c_str());
    cJSON_AddStringToObject(json, "name", station.name.c_str());
    cJSON_AddStringToObject(json, "url", station.url.c_str());
    cJSON_AddStringToObject(json, "genre", station.genre.c_str());
    cJSON_AddStringToObject(json, "country", station.country.c_str());
    cJSON_AddNumberToObject(json, "bitrate", station.bitrate);
    cJSON_AddBoolToObject(json, "favorite", station.favorite);
    cJSON_AddNumberToObject(json, "play_count", station.play_count);
    cJSON_AddNumberToObject(json, "last_played", station.last_played);
    cJSON_AddNumberToObject(json, "gain", station.gain);
    
    char* json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    if (json_str) {
        err = nvs_set_str(handle, key, json_str);
        free(json_str);
    } else {
        err = ESP_ERR_NO_MEM;
    }
    
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool StationManager::LoadStationsFromNvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    
    // Get count
    size_t count = 0;
    err = nvs_get_u32(handle, kKeyCount, reinterpret_cast<uint32_t*>(&count));
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    
    stations_.clear();
    stations_.reserve(count);
    
    for (size_t i = 0; i < count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "%s%d", kKeyPrefix, static_cast<int>(i));
        
        size_t len = 0;
        err = nvs_get_str(handle, key, nullptr, &len);
        if (err != ESP_OK) continue;
        
        std::string json_str(len, '\0');
        err = nvs_get_str(handle, key, &json_str[0], &len);
        if (err != ESP_OK) continue;
        
        cJSON* json = cJSON_Parse(json_str.c_str());
        if (!json) continue;
        
        Station station;
        cJSON* id = cJSON_GetObjectItem(json, "id");
        cJSON* name = cJSON_GetObjectItem(json, "name");
        cJSON* url = cJSON_GetObjectItem(json, "url");
        cJSON* genre = cJSON_GetObjectItem(json, "genre");
        cJSON* country = cJSON_GetObjectItem(json, "country");
        cJSON* bitrate = cJSON_GetObjectItem(json, "bitrate");
        cJSON* favorite = cJSON_GetObjectItem(json, "favorite");
        cJSON* play_count = cJSON_GetObjectItem(json, "play_count");
        cJSON* last_played = cJSON_GetObjectItem(json, "last_played");
        cJSON* gain = cJSON_GetObjectItem(json, "gain");
        
        if (cJSON_IsString(id)) station.id = id->valuestring;
        if (cJSON_IsString(name)) station.name = name->valuestring;
        if (cJSON_IsString(url)) station.url = url->valuestring;
        if (cJSON_IsString(genre)) station.genre = genre->valuestring;
        if (cJSON_IsString(country)) station.country = country->valuestring;
        if (cJSON_IsNumber(bitrate)) station.bitrate = bitrate->valueint;
        if (cJSON_IsBool(favorite)) station.favorite = cJSON_IsTrue(favorite);
        if (cJSON_IsNumber(play_count)) station.play_count = play_count->valueint;
        if (cJSON_IsNumber(last_played)) station.last_played = last_played->valueint;
        if (cJSON_IsNumber(gain)) station.gain = std::clamp(gain->valueint, 50, 300);
        else station.gain = 100;  // Default for old entries without gain
        
        cJSON_Delete(json);
        
        if (!station.name.empty() && !station.url.empty()) {
            stations_.push_back(std::move(station));
        }
    }
    
    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded %zu stations from NVS", stations_.size());
    return !stations_.empty();
}

bool StationManager::SaveAllStationsToNvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(nvs_namespace_.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    
    // Save count
    err = nvs_set_u32(handle, kKeyCount, stations_.size());
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    
    // Save each station
    for (size_t i = 0; i < stations_.size(); i++) {
        if (!SaveStationToNvs(stations_[i], static_cast<int>(i))) {
            nvs_close(handle);
            return false;
        }
    }
    
    // Remove old entries if any
    char key[32];
    for (size_t i = stations_.size(); i < stations_.size() + 10; i++) {
        snprintf(key, sizeof(key), "%s%d", kKeyPrefix, static_cast<int>(i));
        nvs_erase_key(handle, key);
    }
    
    err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

std::string StationManager::GenerateId() const {
    static uint32_t counter = 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "st_%lu_%lu", static_cast<unsigned long>(time(nullptr)), counter++);
    return std::string(buf);
}

std::optional<StationManager::Station> StationManager::GetNextStation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stations_.empty()) return std::nullopt;
    
    // Most recently played station, not the first one ever played.
    auto it = std::max_element(stations_.begin(), stations_.end(),
                               [](const Station& a, const Station& b) {
                                   return a.last_played < b.last_played;
                               });
    if (it == stations_.end() || it->last_played == 0) {
        return stations_.front();  // Return first station if none played
    }
    
    auto next = std::next(it);
    if (next == stations_.end()) {
        return stations_.front();  // Wrap around
    }
    return *next;
}

std::optional<StationManager::Station> StationManager::GetPrevStation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stations_.empty()) return std::nullopt;
    
    // Most recently played station, not the first one ever played.
    auto it = std::max_element(stations_.begin(), stations_.end(),
                               [](const Station& a, const Station& b) {
                                   return a.last_played < b.last_played;
                               });
    if (it == stations_.end() || it->last_played == 0) {
        return stations_.back();  // Return last station if none played
    }
    
    if (it == stations_.begin()) {
        return stations_.back();  // Wrap around
    }
    return *std::prev(it);
}

}  // namespace radio
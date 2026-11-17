#include "radio_web_server.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "application.h"
#include "settings.h"
#include "wifi_manager.h"
#include "radio_source.h"
#include "station_manager.h"

namespace radio {

static const char* TAG = "RadioWeb";

static std::shared_ptr<RadioSource> radio_source_;
static httpd_handle_t server_ = nullptr;
static TaskHandle_t monitor_task_ = nullptr;

// Comma-separated mirror hostnames, persisted in NVS ("radio_web" namespace).
static constexpr const char* kSettingsNs = "radio_web";
static constexpr const char* kMirrorsKey = "mirrors";
static constexpr const char* kDefaultMirrors = "";

extern const char radio_index_html_start[] asm("_binary_radio_index_html_start");
extern const char radio_index_html_end[] asm("_binary_radio_index_html_end");

// ---------- helpers ----------

static esp_err_t SendJson(httpd_req_t* req, cJSON* json) {
    char* str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!str) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    httpd_resp_set_type(req, "application/json");
    // 系统只有 10 个 lwIP socket；立刻断开连接释放 fd，避免电台推流时 accept EMFILE
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t err = httpd_resp_send(req, str, strlen(str));
    free(str);
    return err;
}

static esp_err_t SendError(httpd_req_t* req, httpd_err_code_t code, const char* msg) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    std::string body = std::string("{\"error\":\"") + msg + "\"}";
    return httpd_resp_send_err(req, code, body.c_str());
}

static std::string ReadBody(httpd_req_t* req) {
    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 2048) return "";
    std::string body;
    body.resize(remaining);
    int received = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, &body[received], remaining);
        if (n <= 0) return "";
        received += n;
        remaining -= n;
    }
    return body;
}

// Read a URL query value into `out`. Returns false when absent.
static bool GetQueryKey(httpd_req_t* req, const char* key, std::string* out) {
    size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len <= 1) return false;
    std::string query(len, '\0');
    if (httpd_req_get_url_query_str(req, &query[0], len) != ESP_OK) return false;
    char val[64] = {0};
    if (httpd_query_key_value(query.c_str(), key, val, sizeof(val)) != ESP_OK) return false;
    *out = std::string(val);
    return true;
}

// ---------- NVS station list ----------

static esp_err_t HandleStationsGet(httpd_req_t* req) {
    auto stations = radio_source_->GetStationManager().GetAllStations();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "max", StationManager::kMaxStations);
    cJSON* arr = cJSON_AddArrayToObject(o, "stations");
    int index = 1;
    for (const auto& s : stations) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", index++);
        cJSON_AddStringToObject(o, "id", s.id.c_str());
        cJSON_AddStringToObject(o, "name", s.name.c_str());
        cJSON_AddStringToObject(o, "url", s.url.c_str());
        cJSON_AddStringToObject(o, "genre", s.genre.c_str());
        cJSON_AddStringToObject(o, "country", s.country.c_str());
        cJSON_AddNumberToObject(o, "gain", s.gain);
        cJSON_AddItemToArray(arr, o);
    }
    return SendJson(req, o);
}

static esp_err_t HandleStationsPost(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    auto cleanup = [&root]() { cJSON_Delete(root); };

    auto field = [&](const char* key) -> std::string {
        cJSON* v = cJSON_GetObjectItem(root, key);
        return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
    };
    std::string name = field("name");
    std::string url = field("url");
    std::string genre = field("genre");
    std::string country = field("country");
    cJSON* gain_item = cJSON_GetObjectItem(root, "gain");
    int gain = cJSON_IsNumber(gain_item) ? std::clamp(gain_item->valueint, 50, 300) : 100;

    if (name.empty() || url.empty()) {
        cleanup();
        return SendError(req, HTTPD_400_BAD_REQUEST, "name and url required");
    }
    // Only HTTP(S) streams are playable by the device pipeline.
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        cleanup();
        return SendError(req, HTTPD_400_BAD_REQUEST, "url must be http(s)");
    }

    auto& mgr = radio_source_->GetStationManager();
    if (mgr.GetAllStations().size() >= StationManager::kMaxStations) {
        cleanup();
        std::string msg = "station list full (max " + std::to_string(StationManager::kMaxStations) + ")";
        return SendError(req, HTTPD_400_BAD_REQUEST, msg.c_str());
    }
    if (mgr.FindByUrl(url)) {
        cleanup();
        return SendError(req, HTTPD_400_BAD_REQUEST, "station already saved");
    }

    StationManager::Station station;
    station.name = name;
    station.url = url;
    station.genre = genre;
    station.country = country;
    station.gain = gain;
    cleanup();

    if (!mgr.AddStation(station)) {
        return SendError(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save");
    }
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

static esp_err_t HandleStationsDelete(httpd_req_t* req) {
    std::string index_str;
    if (!GetQueryKey(req, "index", &index_str) || index_str.empty()) {
        return SendError(req, HTTPD_400_BAD_REQUEST, "index required");
    }
    int index = std::atoi(index_str.c_str());
    auto stations = radio_source_->GetStationManager().GetAllStations();
    if (index < 1 || index > static_cast<int>(stations.size())) {
        return SendError(req, HTTPD_400_BAD_REQUEST, "index out of range");
    }
    const auto& target = stations[index - 1];
    // Deleting the station that is on air would leave a nameless stream going.
    if (Application::GetInstance().IsRadioPlaying() &&
        radio_source_->GetCurrentUrl() == target.url) {
        Application::GetInstance().StopRadio();
    }
    if (!radio_source_->GetStationManager().RemoveStation(target.id)) {
        return SendError(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to delete");
    }
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

static esp_err_t HandleStationsGain(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    cJSON* index_item = cJSON_GetObjectItem(root, "index");
    cJSON* gain_item = cJSON_GetObjectItem(root, "gain");
    if (!cJSON_IsNumber(index_item) || !cJSON_IsNumber(gain_item)) {
        cJSON_Delete(root);
        return SendError(req, HTTPD_400_BAD_REQUEST, "index and gain required");
    }
    int index = index_item->valueint;
    int gain = std::clamp(gain_item->valueint, 50, 300);
    cJSON_Delete(root);

    auto& mgr = radio_source_->GetStationManager();
    auto stations = mgr.GetAllStations();
    if (index < 1 || index > static_cast<int>(stations.size())) {
        return SendError(req, HTTPD_400_BAD_REQUEST, "index out of range");
    }
    StationManager::Station station = stations[index - 1];
    station.gain = gain;
    if (!mgr.UpdateStation(station)) {
        return SendError(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to update");
    }
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

// ---------- playback ----------

static esp_err_t HandlePreview(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    cJSON* url_item = cJSON_GetObjectItem(root, "url");
    std::string url = cJSON_IsString(url_item) && url_item->valuestring ? url_item->valuestring : "";
    cJSON_Delete(root);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return SendError(req, HTTPD_400_BAD_REQUEST, "url must be http(s)");
    }
    // Same pipeline as voice radio.play: mic mute, watchdog, state machine.
    // Starting another preview simply switches the station (StartRadio stops
    // the current one first). Not persisted to NVS.
    Application::GetInstance().StartRadio(url);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

static esp_err_t HandleStop(httpd_req_t* req) {
    Application::GetInstance().StopRadio();
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

static esp_err_t HandleVolume(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    cJSON* vol_item = cJSON_GetObjectItem(root, "volume");
    if (!cJSON_IsNumber(vol_item)) {
        cJSON_Delete(root);
        return SendError(req, HTTPD_400_BAD_REQUEST, "volume required");
    }
    int volume = std::clamp(vol_item->valueint, 0, 100);
    cJSON_Delete(root);
    radio_source_->SetVolume(volume);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

static esp_err_t HandleStatus(httpd_req_t* req) {
    auto& app = Application::GetInstance();
    bool playing = app.IsRadioPlaying();
    std::string name;
    std::string state = "stopped";
    if (playing) {
        switch (radio_source_->GetState()) {
            case RadioState::kConnecting: state = "connecting"; break;
            case RadioState::kBuffering: state = "buffering"; break;
            case RadioState::kPlaying: state = "playing"; break;
            case RadioState::kError: state = "error"; break;
            default: state = "stopped"; break;
        }
        auto station = radio_source_->GetStationManager().FindByUrl(radio_source_->GetCurrentUrl());
        if (station) name = station->name;
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "playing", playing);
    cJSON_AddStringToObject(o, "state", state.c_str());
    cJSON_AddStringToObject(o, "name", name.c_str());
    return SendJson(req, o);
}

static esp_err_t HandleConfigPut(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    cJSON* arr = cJSON_GetObjectItem(root, "mirrors");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return SendError(req, HTTPD_400_BAD_REQUEST, "mirrors array required");
    }
    std::string joined;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsString(item) || !item->valuestring) continue;
        if (!joined.empty()) joined += ",";
        joined += item->valuestring;
    }
    cJSON_Delete(root);
    Settings settings(kSettingsNs, true);
    settings.SetString(kMirrorsKey, joined);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

static esp_err_t HandleStationsPut(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    cJSON* index_item = cJSON_GetObjectItem(root, "index");
    cJSON* url_item = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsNumber(index_item) || !cJSON_IsString(url_item) || !url_item->valuestring) {
        cJSON_Delete(root);
        return SendError(req, HTTPD_400_BAD_REQUEST, "index and url required");
    }
    int index = index_item->valueint;
    std::string url = url_item->valuestring;
    cJSON_Delete(root);

    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return SendError(req, HTTPD_400_BAD_REQUEST, "url must be http(s)");
    }

    auto& mgr = radio_source_->GetStationManager();
    auto stations = mgr.GetAllStations();
    if (index < 1 || index > static_cast<int>(stations.size())) {
        return SendError(req, HTTPD_400_BAD_REQUEST, "index out of range");
    }
    StationManager::Station station = stations[index - 1];
    station.url = url;
    if (!mgr.UpdateStation(station)) {
        return SendError(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to update");
    }
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

static esp_err_t HandleStationsOrder(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    cJSON* index_item = cJSON_GetObjectItem(root, "index");
    cJSON* dir_item = cJSON_GetObjectItem(root, "dir");
    if (!cJSON_IsNumber(index_item) || !cJSON_IsString(dir_item) || !dir_item->valuestring) {
        cJSON_Delete(root);
        return SendError(req, HTTPD_400_BAD_REQUEST, "index and dir required");
    }
    int index = index_item->valueint;
    // Copy before cJSON_Delete: dir_item->valuestring is freed with root.
    std::string dir = dir_item->valuestring;
    bool valid = dir == "up" || dir == "down" || dir == "top";
    cJSON_Delete(root);
    if (!valid) return SendError(req, HTTPD_400_BAD_REQUEST, "dir must be up|down|top");
    if (!radio_source_->GetStationManager().MoveStation(index, dir.c_str())) {
        return SendError(req, HTTPD_400_BAD_REQUEST, "move failed");
    }
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

// ---------- mirror config ----------

static esp_err_t HandleConfigGet(httpd_req_t* req) {
    Settings settings(kSettingsNs, false);
    std::string mirrors = settings.GetString(kMirrorsKey, kDefaultMirrors);
    cJSON* o = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();
    size_t start = 0;
    while (start <= mirrors.size()) {
        size_t comma = mirrors.find(',', start);
        std::string item = mirrors.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        // Trim whitespace
        size_t first = item.find_first_not_of(" \t");
        if (first != std::string::npos) {
            item = item.substr(first, item.find_last_not_of(" \t") - first + 1);
            if (!item.empty()) cJSON_AddItemToArray(arr, cJSON_CreateString(item.c_str()));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    cJSON_AddItemToObject(o, "mirrors", arr);
    return SendJson(req, o);
}

// ---------- index page ----------

static esp_err_t HandleIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, radio_index_html_start,
                           strlen(radio_index_html_start));
}

// ---------- server lifecycle ----------

static const httpd_uri_t kUris[] = {
    {"/", HTTP_GET, HandleIndex, nullptr},
    {"/api/stations", HTTP_GET, HandleStationsGet, nullptr},
    {"/api/stations", HTTP_POST, HandleStationsPost, nullptr},
    {"/api/stations", HTTP_DELETE, HandleStationsDelete, nullptr},
    {"/api/stations", HTTP_PUT, HandleStationsPut, nullptr},
    {"/api/stations/gain", HTTP_POST, HandleStationsGain, nullptr},
    {"/api/stations/order", HTTP_POST, HandleStationsOrder, nullptr},
    {"/api/preview", HTTP_POST, HandlePreview, nullptr},
    {"/api/stop", HTTP_POST, HandleStop, nullptr},
    {"/api/volume", HTTP_POST, HandleVolume, nullptr},
    {"/api/status", HTTP_GET, HandleStatus, nullptr},
    {"/api/config", HTTP_GET, HandleConfigGet, nullptr},
    {"/api/config", HTTP_PUT, HandleConfigPut, nullptr},
};

static bool StartServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    if (httpd_start(&server_, &config) != ESP_OK) {
        server_ = nullptr;
        return false;
    }
    for (const auto& uri : kUris) {
        if (httpd_register_uri_handler(server_, &uri) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s", uri.uri);
        }
    }
    ESP_LOGI(TAG, "Radio web server started on port 80");
    return true;
}

// Keeps the server up except in WiFi config mode, where the provisioning AP
// needs port 80. Restarted once the device leaves that mode.
static void MonitorTask(void*) {
    bool running = false;
    for (;;) {
        // Wake instantly on state changes; the 1 s timeout also drives the
        // initial start attempt after boot.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        bool desired = Application::GetInstance().GetDeviceState() != kDeviceStateWifiConfiguring;
        if (desired && !running) {
            running = StartServer();
        } else if (!desired && running) {
            ESP_LOGI(TAG, "WiFi config mode, stopping radio web server");
            httpd_stop(server_);
            server_ = nullptr;
            running = false;
        }
    }
}

std::string RadioWebServer::GetWebPageUrl() {
    std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (ip.empty()) return "";
    return "http://" + ip + "/";
}

void RadioWebServer::Start(std::shared_ptr<RadioSource> source) {    radio_source_ = source;
    if (xTaskCreate(MonitorTask, "radio_web", 4096, nullptr, 3, &monitor_task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        return;
    }
    Application::GetInstance().AddStateChangeListener([](DeviceState, DeviceState new_state) {
        // Fires in the TransitionTo caller's context: only wake the monitor.
        if (new_state == kDeviceStateWifiConfiguring && monitor_task_) {
            xTaskNotifyGive(monitor_task_);
        }
    });
}

}  // namespace radio

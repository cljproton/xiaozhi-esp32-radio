#pragma once

#include <memory>

namespace radio {

class RadioSource;

// Persistent HTTP server (port 80) for managing radio stations from a
// browser: search radio-browser.info, add/remove stations, preview streams
// and tune per-station gain. The server pauses itself while the device is
// in WiFi config mode so the provisioning AP can bind port 80.
class RadioWebServer {
public:
    // Spawns a monitor task; safe to call once at startup.
    static void Start(std::shared_ptr<RadioSource> source);

    // "http://<device ip>/" for the station management page, or "" when the
    // device has no station IP yet.
    static std::string GetWebPageUrl();
};

}  // namespace radio

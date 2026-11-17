#pragma once

#include <string>

namespace radio {

struct StationInfo {
    std::string name;
    std::string url;
    std::string genre;
    std::string country;
    int bitrate = 0;           // kbps
    bool favorite = false;
    int gain = 100;            // playback gain percentage (100 = normal)
};

}  // namespace radio
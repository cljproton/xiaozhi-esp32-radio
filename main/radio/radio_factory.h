#pragma once

#include "radio_source.h"
#include <memory>

namespace radio {

// Factory function to create radio source
std::shared_ptr<RadioSource> CreateRadioSource();

}  // namespace radio
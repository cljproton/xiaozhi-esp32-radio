#include "radio_factory.h"

// The radio source implementation is header-only included here so the whole
// factory (class + CreateRadioSource) lives in a single translation unit.
#include "radio_source_esp_adf.cc"

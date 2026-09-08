#pragma once
#include "platform.h"
#include <string>
#include <cstdint>
namespace frzz {
struct MediaMetadata {double duration{};int rate{},channels{},format_major{};std::int64_t frames{},packets{};std::string codec,expected_codec,error;};
bool scan_media(const char* path,MediaMetadata& metadata);
bool scan_ff59(const char*,ModuleHandle,ModuleHandle,MediaMetadata&);
bool scan_ff60(const char*,ModuleHandle,ModuleHandle,MediaMetadata&);
bool scan_ff61(const char*,ModuleHandle,ModuleHandle,MediaMetadata&);
bool scan_ff62(const char*,ModuleHandle,ModuleHandle,MediaMetadata&);
}

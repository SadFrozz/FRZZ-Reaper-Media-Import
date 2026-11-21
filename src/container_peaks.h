#pragma once

#include "reaper_plugin.h"
#include <cstdint>

namespace frzz {
enum class PeakFileStatus { missing, complete, incomplete };
PeakFileStatus inspect_peak_file(const char* path,int channels,std::uint32_t rate,std::int64_t frames,
                                 const char* media=nullptr,bool exact_duration=false);
bool complete_peak_file(const char* path,int channels,std::uint32_t rate,std::int64_t frames,const char* media=nullptr);
void read_peak_file(const char* path,int channels,std::uint32_t rate,std::int64_t frames,PCM_source_peaktransfer_t& block);
}

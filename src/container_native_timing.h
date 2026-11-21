#pragma once

#include "platform.h"
#include <vector>

namespace frzz {
struct NativeTimingSlots {
  ModuleHandle image;
  unsigned format_open;
  unsigned open_return;
  unsigned avutil_malloc;
};
struct NativeTimingHook {
  bool (*install)(NativeTimingSlots);
  void (*select)(const void*,int,int,std::int64_t,const char*,const std::vector<int>*) noexcept;
  bool (*enable_channel_projection)(const void*,int,int) noexcept;
  void (*shutdown)() noexcept;
};
#ifdef _WIN32
extern const NativeTimingHook native_timing_62;
#endif
#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))
extern const NativeTimingHook native_timing_63;
#endif
}

#pragma once

#include "platform.h"
#include <vector>

namespace frzz {
struct NativeTimingSlots {
  ModuleHandle image;
  unsigned format_open;
  unsigned open_return;
};
struct NativeTimingHook {
  int format_major;
  bool (*install)(NativeTimingSlots);
  bool (*planar_bed)(const void*,int,int) noexcept;
  void (*select)(const void*,int,int,std::int64_t,const char*,const std::vector<int>*) noexcept;
  bool (*enable_channel_projection)(const void*,int,int) noexcept;
  void (*shutdown)() noexcept;
};
#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))
extern const NativeTimingHook native_timing_59;
extern const NativeTimingHook native_timing_60;
extern const NativeTimingHook native_timing_61;
extern const NativeTimingHook native_timing_62;
extern const NativeTimingHook native_timing_63;
inline constexpr const NativeTimingHook* native_timing_adapters[]{
  &native_timing_63,&native_timing_62,&native_timing_61,&native_timing_60,&native_timing_59,
};
#endif
}

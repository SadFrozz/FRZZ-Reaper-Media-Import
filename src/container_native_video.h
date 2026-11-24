#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frzz {
enum class NativeVideoTrackUse { decoder, peaks };

class NativeVideoTrackCheck {
#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))
  std::string_view alias_;
  NativeVideoTrackCheck* previous_{};
#endif
  bool used_{};
  bool timing_used_{};
  bool projection_used_{};
public:
#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))
  explicit NativeVideoTrackCheck(std::string_view alias) noexcept;
  ~NativeVideoTrackCheck();
  static void observe(std::string_view alias) noexcept;
  static void observe_timing(std::string_view alias) noexcept;
  static void observe_projection(std::string_view alias) noexcept;
#else
  explicit NativeVideoTrackCheck(std::string_view) noexcept {}
#endif
  NativeVideoTrackCheck(const NativeVideoTrackCheck&)=delete;
  NativeVideoTrackCheck& operator=(const NativeVideoTrackCheck&)=delete;
  bool used() const {return used_;}
  bool timing_used() const {return timing_used_;}
  bool projection_used() const {return projection_used_;}
};

#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))
void set_native_video_peak_api(void* peak_name,void* peak_ex,void* peak_legacy);
void shutdown_native_video_tracks();
std::string native_video_track_path(const std::string& file,int stream_index,int audio_ordinal,
                                    int source_channels,int main_channels,NativeVideoTrackUse use,int timing_rate=0,std::int64_t timing_frames=0,const std::vector<int>* projection=nullptr);
bool is_native_video_track_path(const char* path);
bool native_video_track_verified(const std::string& alias,std::string_view decoder_settings);
void verify_native_video_track(const std::string& alias,std::string_view decoder_settings);
#else
inline void set_native_video_peak_api(void*,void*,void*) {}
inline void shutdown_native_video_tracks() {}
inline std::string native_video_track_path(const std::string&,int,int,int,int,NativeVideoTrackUse,int=0,std::int64_t=0,const std::vector<int>* =nullptr){return {};}
inline bool is_native_video_track_path(const char*){return false;}
inline bool native_video_track_verified(const std::string&,std::string_view){return false;}
inline void verify_native_video_track(const std::string&,std::string_view){}
#endif

#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))
void poll_native_hardware_video();
#else
inline void poll_native_hardware_video() {}
#endif
#ifdef _WIN32
bool windows_hardware_video_supported() noexcept;
#endif
}

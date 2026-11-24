#pragma once

#include <cstdint>

namespace frzz {
struct VideoSettings {
  bool multiple_audio_tracks{true};
  bool hdr_to_sdr{true};
  bool hevc_patches{true};
  bool skip_hevc_seek_frames{true};
  std::uint32_t seek_generation{};

  bool hevc_seek_optimization() const noexcept {return hevc_patches && skip_hevc_seek_frames;}
};

VideoSettings video_settings() noexcept;
void load_video_settings();
void prepare_video_hdr_shaders() noexcept;
int register_video_preferences(void* instance,int (*register_api)(const char*,void*));
void unregister_video_preferences(int (*register_api)(const char*,void*));
}

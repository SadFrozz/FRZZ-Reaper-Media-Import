#pragma once
#include "platform.h"
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace frzz {
struct ContainerTimeline {
  struct Anchor {
    std::int64_t frame;
    std::int64_t timestamp_offset;
    int cadence_probe_frames;
    bool native_timestamps;
  };
  std::mutex mutex;
  std::map<std::int64_t,Anchor> anchors;
  std::mutex eac3_grid_mutex;
  std::optional<bool> eac3_grid;
};
struct ContainerStream {
  int index{}, channels{}, source_channels{}, rate{};
  bool default_track{};
  bool native_planar_bed{};
  std::uint32_t track_id{};
  std::int64_t frames{};
  std::int64_t start_offset_frames{};
  double origin{};
  std::shared_ptr<ContainerTimeline> timeline;
  std::string language, title, codec, channel_order;
  std::vector<int> channel_map;
  std::vector<int> bed_speakers;
};
struct ContainerAudio {
  double duration{};
  bool quicktime{};
  bool has_video{};
  std::vector<ContainerStream> streams;
};
struct ContainerModules {
  ModuleHandle format{},codec{},util{};
  ~ContainerModules(){release_module(format);release_module(codec);release_module(util);}
};
class ContainerDecoder {
public:
  virtual ~ContainerDecoder()=default;
  virtual int read(std::int64_t position,double* output,int frames)=0;
};
bool probe_container(const char* path,const std::filesystem::path& resource,ContainerAudio& info);
int default_container_stream(const std::vector<ContainerStream>& streams);
std::unique_ptr<ContainerDecoder> open_container(const char* path,const std::filesystem::path& resource,const ContainerStream& stream);

using ContainerProbe=bool(*)(const char*,const ContainerModules&,ContainerAudio&);
using ContainerOpen=ContainerDecoder*(*)(const char*,std::shared_ptr<ContainerModules>,const ContainerStream&);
}

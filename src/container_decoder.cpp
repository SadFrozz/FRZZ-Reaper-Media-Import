#include "container_decoder.h"
#include "mov_track_metadata.h"
#include <algorithm>
#include <array>

namespace frzz {
#define ADAPTER(n) bool probe_container_##n(const char*,const ContainerModules&,ContainerAudio&); \
  ContainerDecoder* open_container_##n(const char*,std::shared_ptr<ContainerModules>,const ContainerStream&);
ADAPTER(59)
ADAPTER(60)
ADAPTER(61)
ADAPTER(62)
ADAPTER(63)
#undef ADAPTER

struct Adapter {int major;ContainerProbe probe;ContainerOpen open;};
static constexpr std::array<Adapter,5> adapters{{
  {63,probe_container_63,open_container_63},{62,probe_container_62,open_container_62},
  {61,probe_container_61,open_container_61},{60,probe_container_60,open_container_60},
  {59,probe_container_59,open_container_59}
}};

static ModuleHandle load_from_plugin_dir(const std::filesystem::path& dir,const char* family,int major) {
  if(auto module=retain_loaded_module(family,major))return module;
#ifdef _WIN32
  const auto file=dir/(std::string(family)+"-"+std::to_string(major)+".dll");
  return LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
  const auto file=dir/("lib"+std::string(family)+"."+std::to_string(major)+".dylib");
  return dlopen(file.c_str(),RTLD_LOCAL|RTLD_LAZY);
#endif
}

static std::shared_ptr<ContainerModules> bind(const std::filesystem::path& resource,const Adapter& adapter) {
  for(const auto& dir:{resource/"UserPlugins",resource/"UserPlugins"/"FFmpeg"}) {
    auto modules=std::make_shared<ContainerModules>();
    modules->format=load_from_plugin_dir(dir,"avformat",adapter.major);
    if(!modules->format)continue;
    modules->codec=load_from_plugin_dir(dir,"avcodec",adapter.major);
    modules->util=load_from_plugin_dir(dir,"avutil",adapter.major-2);
    if(modules->codec && modules->util)return modules;
  }
  return {};
}

bool probe_container(const char* path,const std::filesystem::path& resource,ContainerAudio& info) {
  for(const auto& adapter:adapters) {
    if(auto modules=bind(resource,adapter)) {
      if(!adapter.probe(path,*modules,info))return false;
      if(info.quicktime && info.streams.size()>1)read_mov_track_metadata(path,info);
      return true;
    }
  }
  return false;
}

int default_container_stream(const std::vector<ContainerStream>& streams) {
  const auto preferred=std::find_if(streams.begin(),streams.end(),[](const auto& track){return track.default_track && track.frames>0;});
  if(preferred!=streams.end())return preferred->index;
  const auto first=std::find_if(streams.begin(),streams.end(),[](const auto& track){return track.frames>0;});
  return first==streams.end()?-2:first->index;
}

std::unique_ptr<ContainerDecoder> open_container(const char* path,const std::filesystem::path& resource,const ContainerStream& stream) {
  for(const auto& adapter:adapters)
    if(auto modules=bind(resource,adapter))
      return std::unique_ptr<ContainerDecoder>(adapter.open(path,std::move(modules),stream));
  return {};
}
}

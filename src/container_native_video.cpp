#include "platform.h"
#include "container_native_video.h"
#include "container_native_timing.h"
#include "support.h"
#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))
#ifdef _WIN32
#include <MinHook.h>
#include <wincrypt.h>
#endif
#include <atomic>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace frzz {
extern "C" void frzz_video_decoder_hook();
extern "C" void frzz_video_peak_hook();
extern "C" void frzz_video_channels_hook();
extern "C" {
void* frzz_original_open{};
void* frzz_original_peak_name{};
void* frzz_original_channels{};
}
bool native_planar_bed_63(const void* codec_context,int source_channels,int main_channels) noexcept;
#ifdef _WIN32
bool native_planar_bed_62(const void* codec_context,int source_channels,int main_channels) noexcept;
#endif
#ifdef __APPLE__
bool install_macos_video_hooks(void* peak_name,void* peak_ex,void* peak_legacy);
bool install_macos_video_timing();
#endif
namespace {
using PlanarBedCheck=bool (*)(const void*,int,int) noexcept;
#ifdef _WIN32
// Native FFmpeg decoder factory and PCM peak naming API on Windows x64.
void* decoder_target{};
void* channels_target{};
HMODULE inspected_module{};

struct KnownVideoPlugin {
  std::string_view sha256;
  std::uint32_t decoder;
  std::uint32_t channel_count;
  PlanarBedCheck check_bed;
  const NativeTimingHook& timing;
  unsigned format_open;
  unsigned open_return;
};
constexpr KnownVideoPlugin known_plugins[]{
  {"a216e95294cb4ea7788ab486b09b1cf81b9271e4eaa3d34d353d69247e3fa4aa",0x7c0f4,0x7c4f8,native_planar_bed_62,native_timing_62,0x2406a8,0x7c2a2}, // 7.66 x64
  {"d8526365a3c4b9714c2c49f52a7c1a17bd40e17efae58d763270f7a26f143c71",0x8452c,0x84930,native_planar_bed_62,native_timing_62,0x2517a8,0x846da}, // 7.77 x64
  {"ebe822c5ee9dbb32394dc184a5d40c29fd4b6fc80b6f217ae9be9ad4cf33fddf",0x8fda4,0x901a8,native_planar_bed_63,native_timing_63,0x266888,0x8ff52}, // 7.85 x64
  {"3012d1a65f9daa8d7cd390be1d892f71fa2907e066cfc9b00f70eb236c240913",0x8fda4,0x901a8,native_planar_bed_63,native_timing_63,0x266888,0x8ff52}, // 7.86 x64
};
PlanarBedCheck check_bed{};
const KnownVideoPlugin* active_video_plugin{};
#else
constexpr PlanarBedCheck check_bed=native_planar_bed_63;
#endif

void* peak_target{};
#ifdef __APPLE__
void* peak_ex_target{};
void* peak_legacy_target{};
std::atomic<bool> routing_enabled{};
#endif
bool hooks_installed{};
bool hooks_stop_requested{};
std::mutex hooks_mutex;
thread_local NativeVideoTrackCheck* current_check{};
struct ChannelChoice {
  const void* factory_frame{};
  std::string_view alias;
  int source_channels{};
  int main_channels{};
  bool project_channels{};
};
thread_local std::array<ChannelChoice,8> channel_choices;
thread_local size_t channel_choice_count{};

struct Track {
  std::string file;
  std::string peak_extension;
  std::string alias;
  int ordinal;
  int index;
  int source_channels;
  int main_channels;
  std::filesystem::file_time_type modified;
  std::uintmax_t size;
  std::string verified_settings;
  bool decoder_enabled{};
  int timing_rate{};
  std::int64_t timing_frames{};
  std::unique_ptr<const std::vector<int>> projection;
};
std::shared_mutex tracks_mutex;
std::map<std::string,std::shared_ptr<Track>,std::less<>> tracks;

std::shared_ptr<const Track> find_track(const char* path,NativeVideoTrackUse use=NativeVideoTrackUse::peaks,
                                      int* timing_rate=nullptr,std::int64_t* timing_frames=nullptr,
                                      const std::vector<int>** projection=nullptr) {
  if(!path)return {};
  const std::string_view filename(path);
#ifdef _WIN32
  if(filename.find("\\.\\.\\")==filename.npos)return {};
#else
  if(!routing_enabled.load(std::memory_order_acquire) || filename.find("/././")==filename.npos)return {};
#endif
  std::shared_lock<std::shared_mutex> lock(tracks_mutex);
  const auto found=tracks.find(filename);
  if(found==tracks.end() || (use==NativeVideoTrackUse::decoder && !found->second->decoder_enabled))return {};
  if(timing_rate)*timing_rate=found->second->timing_rate;
  if(timing_frames)*timing_frames=found->second->timing_frames;
  if(projection)*projection=found->second->projection.get();
  return found->second;
}

#ifdef _WIN32
std::string module_sha256(HMODULE module) {
  std::wstring path(32768,L'\0');
  const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
  if(!length || length>=path.size())return {};
  path.resize(length);
  std::ifstream file(std::filesystem::path(path),std::ios::binary|std::ios::ate);
  const auto bytes=file.tellg();
  if(bytes<=0 || bytes>20*1024*1024)return {};
  std::vector<unsigned char> contents(static_cast<std::size_t>(bytes));
  file.seekg(0);
  if(!file.read(reinterpret_cast<char*>(contents.data()),bytes))return {};
  HCRYPTPROV provider{};
  if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT|CRYPT_SILENT))return {};
  HCRYPTHASH state{};
  std::array<unsigned char,32> hash{};
  auto hash_size=static_cast<DWORD>(hash.size());
  const bool okay=CryptCreateHash(provider,CALG_SHA_256,0,0,&state) &&
    CryptHashData(state,contents.data(),static_cast<DWORD>(contents.size()),0) &&
    CryptGetHashParam(state,HP_HASHVAL,hash.data(),&hash_size,0);
  if(state)CryptDestroyHash(state);
  CryptReleaseContext(provider,0);
  if(!okay || hash_size!=hash.size())return {};
  constexpr char hex[]="0123456789abcdef";
  std::string result;
  for(const auto byte:hash){result+=hex[byte>>4];result+=hex[byte&15];}
  return result;
}

bool install_hooks() {
  if(hooks_installed)return true;
  if(!peak_target)return false;
  const auto module=GetModuleHandleW(L"reaper_video.dll");
  if(!module || module==inspected_module)return false;
  inspected_module=module;
  decoder_target=nullptr;
  channels_target=nullptr;
  const auto hash=module_sha256(module);
  for(const auto& plugin:known_plugins) {
    if(plugin.sha256!=hash)continue;
    active_video_plugin=&plugin;
    decoder_target=reinterpret_cast<unsigned char*>(module)+plugin.decoder;
    constexpr unsigned char copy_count[]{0x89,0x59,0x44,0xc7,0x41,0x48,0,0,0,0};
    auto* target=reinterpret_cast<unsigned char*>(module)+plugin.channel_count;
    if(std::memcmp(target,copy_count,sizeof(copy_count))==0) {
      channels_target=target;
      check_bed=plugin.check_bed;
    }
    break;
  }
  if(!decoder_target || MH_Initialize()!=MH_OK)return false;
  if(MH_CreateHook(decoder_target,reinterpret_cast<void*>(frzz_video_decoder_hook),&frzz_original_open)!=MH_OK ||
     MH_CreateHook(peak_target,reinterpret_cast<void*>(frzz_video_peak_hook),&frzz_original_peak_name)!=MH_OK) {
    MH_Uninitialize();
    return false;
  }
  if(channels_target && MH_CreateHook(channels_target,reinterpret_cast<void*>(frzz_video_channels_hook),
                                      &frzz_original_channels)!=MH_OK)channels_target=nullptr;
  // Threads may be returning through a trampoline as REAPER unloads extensions.
  // Keep the bridge and trampolines mapped until the process exits.
  HMODULE self{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
       reinterpret_cast<LPCWSTR>(&frzz_video_decoder_hook),&self)) {
    MH_Uninitialize();
    return false;
  }
  if(MH_QueueEnableHook(decoder_target)!=MH_OK || MH_QueueEnableHook(peak_target)!=MH_OK ||
     (channels_target && MH_QueueEnableHook(channels_target)!=MH_OK) ||
     MH_ApplyQueued()!=MH_OK) {
    MH_QueueDisableHook(decoder_target);
    MH_QueueDisableHook(peak_target);
    if(channels_target)MH_QueueDisableHook(channels_target);
    MH_ApplyQueued();
    return false;
  }
  hooks_installed=true;
  log("native container track routing enabled");
  return true;
}
#else
bool install_hooks() {
  if(hooks_installed)return routing_enabled.load(std::memory_order_acquire);
  if(!peak_target || !peak_ex_target || !peak_legacy_target ||
     !install_macos_video_hooks(peak_target,peak_ex_target,peak_legacy_target))return false;
  hooks_installed=true;
  routing_enabled.store(true,std::memory_order_release);
  log("native container track routing enabled");
  return true;
}
#endif
const NativeTimingHook* video_timing() noexcept {
#ifdef _WIN32
  return active_video_plugin?&active_video_plugin->timing:nullptr;
#else
  return &native_timing_63;
#endif
}
}

extern "C" int frzz_video_ordinal(const char* path,int media_type,int ordinal,const void* factory_frame) noexcept {
  const auto frame=reinterpret_cast<std::uintptr_t>(factory_frame);
  // x86 stacks grow downward. A new call at the same or a higher address
  // discards records from earlier invocations that returned before the copy.
  while(channel_choice_count &&
        reinterpret_cast<std::uintptr_t>(channel_choices[channel_choice_count-1].factory_frame)<=frame)
    --channel_choice_count;
  try {
    int rate=0;
    std::int64_t samples=0;
    const std::vector<int>* projection=nullptr;
    const auto track=media_type==1?find_track(path,NativeVideoTrackUse::decoder,&rate,&samples,&projection):nullptr;
    if(const auto* timing=video_timing())
      timing->select(factory_frame,rate?track->index:-1,rate,samples,rate?track->alias.c_str():nullptr,projection);
    if(track) {
      if(track->source_channels==track->main_channels && !projection)NativeVideoTrackCheck::observe(path);
      else if(channel_choice_count<channel_choices.size())
        channel_choices[channel_choice_count++]={factory_frame,track->alias,track->source_channels,track->main_channels,projection!=nullptr};
    }
    return track?track->ordinal:ordinal;
  }catch(...){return ordinal;}
}

extern "C" int frzz_video_main_channels(const void* factory_frame,const void* codec_context,int channels) noexcept {
  // Tied to the same native invocation, including during nested opens on the
  // same thread. FFmpeg's frames and decoder state remain untouched.
  const auto frame=reinterpret_cast<std::uintptr_t>(factory_frame);
  while(channel_choice_count &&
        reinterpret_cast<std::uintptr_t>(channel_choices[channel_choice_count-1].factory_frame)<frame)
    --channel_choice_count;
  if(!channel_choice_count || channel_choices[channel_choice_count-1].factory_frame!=factory_frame)return channels;
  const auto choice=channel_choices[--channel_choice_count];
  const auto* timing=video_timing();
  if(choice.source_channels==channels &&
     (choice.project_channels?(timing && timing->enable_channel_projection(codec_context,channels,choice.main_channels)):
       (check_bed && check_bed(codec_context,channels,choice.main_channels)))) {
    NativeVideoTrackCheck::observe(choice.alias);
    return choice.main_channels;
  }
  return channels;
}

extern "C" void frzz_video_peak_arguments(const char* path,const char* extension,const char** arguments) noexcept {
  arguments[0]=path;
  arguments[1]=extension;
  if(!extension || std::string_view(extension)!=".reapeaks")return;
  try {
    if(const auto track=find_track(path)) {
      arguments[0]=track->file.c_str();
      arguments[1]=track->peak_extension.c_str();
    }
  }catch(...){}
}

NativeVideoTrackCheck::NativeVideoTrackCheck(std::string_view alias) noexcept
  :alias_(alias),previous_(current_check) {current_check=this;}
NativeVideoTrackCheck::~NativeVideoTrackCheck(){current_check=previous_;}
void NativeVideoTrackCheck::observe(std::string_view alias) noexcept {
  if(current_check && current_check->alias_==alias)current_check->used_=true;
}
void NativeVideoTrackCheck::observe_timing(std::string_view alias) noexcept {
  if(current_check && current_check->alias_==alias)current_check->timing_used_=true;
}
void NativeVideoTrackCheck::observe_projection(std::string_view alias) noexcept {
  if(current_check && current_check->alias_==alias)current_check->projection_used_=true;
}

void set_native_video_peak_api(void* peak_name,[[maybe_unused]] void* peak_ex,[[maybe_unused]] void* peak_legacy) {
  peak_target=peak_name;
#ifdef __APPLE__
  peak_ex_target=peak_ex;
  peak_legacy_target=peak_legacy;
#endif
}

std::string native_video_track_path(const std::string& file,int stream_index,int audio_ordinal,
                                    int source_channels,int main_channels,NativeVideoTrackUse use,int timing_rate,std::int64_t timing_frames,const std::vector<int>* projection) {
  if(stream_index<0 || stream_index>128 || audio_ordinal<0)return {};
  if(main_channels<=0 || source_channels<main_channels)return {};
  const bool adjust_channels=use==NativeVideoTrackUse::decoder && (source_channels>main_channels || projection);
  if(adjust_channels && main_channels>8 && !projection)return {};
  {
    std::lock_guard<std::mutex> lock(hooks_mutex);
    if(hooks_stop_requested || !install_hooks())return {};
#ifdef _WIN32
    if(adjust_channels && !channels_target)return {};
    if(timing_rate>0 && (!active_video_plugin ||
      !active_video_plugin->timing.install({inspected_module,active_video_plugin->format_open,active_video_plugin->open_return,active_video_plugin->format_open+0x80})))
      return {};
#else
    if(timing_rate>0 && !install_macos_video_timing())return {};
#endif
  }
  std::error_code error;
#ifdef _WIN32
  auto path=std::filesystem::absolute(std::filesystem::u8path(file)).lexically_normal().make_preferred();
#else
  auto path=std::filesystem::canonical(std::filesystem::u8path(file),error);
  if(error)return {};
#endif
  const auto modified=std::filesystem::last_write_time(path,error);
  if(error)return {};
  const auto size=std::filesystem::file_size(path,error);
  if(error)return {};
  auto filename=path.parent_path();
  // REAPER keys its jobs by the stored path. The filesystem opens both aliases as the
  // same original file, while the distinct keys keep simultaneous jobs separate.
  for(int i=0;i<=stream_index+1;++i)filename/=".";
  filename/=path.filename();
  auto alias=filename.u8string();
#ifdef _WIN32
  if(std::filesystem::u8path(alias).native().size()>=259)return {};
#else
  if(alias.size()>=1024)return {};
#endif
  auto track=std::shared_ptr<Track>(new Track{path.u8string(),".track"+std::to_string(stream_index)+".reapeaks",alias,
    audio_ordinal,stream_index,source_channels,main_channels,modified,size,{},{},{},{},{}});
  std::unique_lock<std::shared_mutex> lock(tracks_mutex);
  const auto result=tracks.try_emplace(alias,std::move(track));
  auto& saved=*result.first->second;
  if(saved.ordinal!=audio_ordinal || saved.source_channels!=source_channels || saved.main_channels!=main_channels ||
     saved.modified!=modified || saved.size!=size)return {};
  if(use==NativeVideoTrackUse::decoder) {
    if(saved.projection && (!projection || *projection!=*saved.projection))return {};
    if(projection && !saved.projection)saved.projection=std::make_unique<const std::vector<int>>(*projection);
    saved.timing_rate=timing_rate;
    saved.timing_frames=timing_frames;
    saved.decoder_enabled=true;
  }
  return alias;
}

bool is_native_video_track_path(const char* path) {
  return bool(find_track(path));
}

bool native_video_track_verified(const std::string& alias,std::string_view decoder_settings) {
  std::shared_lock<std::shared_mutex> lock(tracks_mutex);
  const auto found=tracks.find(alias);
  return found!=tracks.end() && !found->second->verified_settings.empty() &&
    found->second->verified_settings==decoder_settings;
}

void verify_native_video_track(const std::string& alias,std::string_view decoder_settings) {
  std::unique_lock<std::shared_mutex> lock(tracks_mutex);
  if(const auto found=tracks.find(alias);found!=tracks.end())found->second->verified_settings=decoder_settings;
}

void shutdown_native_video_tracks() {
  std::lock_guard<std::mutex> lock(hooks_mutex);
  hooks_stop_requested=true;
#ifdef _WIN32
  if(!hooks_installed)return;
  if(active_video_plugin)active_video_plugin->timing.shutdown();
  if(MH_QueueDisableHook(decoder_target)!=MH_OK || MH_QueueDisableHook(peak_target)!=MH_OK ||
     (channels_target && MH_QueueDisableHook(channels_target)!=MH_OK) ||
     MH_ApplyQueued()!=MH_OK) {
    log("could not disable native container track routing at shutdown");
    return;
  }
  hooks_installed=false;
#else
  routing_enabled.store(false,std::memory_order_release);
  native_timing_63.shutdown();
#endif
}
}
#endif

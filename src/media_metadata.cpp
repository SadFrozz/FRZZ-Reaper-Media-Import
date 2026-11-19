#include "media_metadata.h"
#include "support.h"
#include <array>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
namespace frzz {
static std::mutex cache_mutex;
static std::map<std::string,MediaMetadata> cache;
static std::string file_key(const char* path) {
#ifdef _WIN32
  const auto wide=std::filesystem::u8path(path).wstring();
  HANDLE file=CreateFileW(wide.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return {};
  BY_HANDLE_FILE_INFORMATION info{};const bool ok=GetFileInformationByHandle(file,&info)!=0;CloseHandle(file);
  if(!ok)return {};
  std::ostringstream key;
  key<<info.dwVolumeSerialNumber<<':'<<info.nFileIndexHigh<<':'<<info.nFileIndexLow<<':'<<info.nFileSizeHigh<<':'<<info.nFileSizeLow<<':'<<info.ftLastWriteTime.dwHighDateTime<<':'<<info.ftLastWriteTime.dwLowDateTime;
#else
  struct stat info{};if(stat(path,&info))return {};
  std::ostringstream key;
  key<<info.st_dev<<':'<<info.st_ino<<':'<<info.st_size<<':'<<info.st_mtimespec.tv_sec<<':'<<info.st_mtimespec.tv_nsec;
#endif
  return key.str();
}
bool scan_media(const char* path,MediaMetadata& metadata) {
  metadata={};
  std::string ext=std::filesystem::u8path(path).extension().u8string();
  for(auto& c:ext)if(c>='A' && c<='Z')c+=('a'-'A');
  if(ext.rfind(".dts",0)==0)metadata.expected_codec="dts";
  else if(ext==".thd" || ext==".truehd" || ext==".thd+ac3")metadata.expected_codec="truehd";
  else if(ext==".mlp")metadata.expected_codec="mlp";
  else if(ext==".ac3")metadata.expected_codec="ac3";
  else if(ext==".ac4")metadata.expected_codec="ac4";
  else if(ext==".eac3" || ext==".ec3" || ext==".ddp")metadata.expected_codec="eac3";
  const auto key=file_key(path);
  if(key.empty()){metadata.error="Cannot identify local source file";return false;}
  // Serialize duplicate source probes, so concurrent opens do not rescan a multi-GB file.
  std::lock_guard<std::mutex> lock(cache_mutex);
  if(auto found=cache.find(key);found!=cache.end()) {
    if(!metadata.expected_codec.empty() && metadata.expected_codec!=found->second.codec){metadata.error="Cached codec does not match extension";return false;}
    metadata=found->second;log("metadata cache hit file=%s duration=%.9f",path,metadata.duration);return true;
  }
  using Scanner=bool(*)(const char*,ModuleHandle,ModuleHandle,MediaMetadata&);
  struct Version { int major; Scanner scan; };
  static constexpr std::array<Version,5> versions{{
    {63,scan_ff63},{62,scan_ff62},{61,scan_ff61},{60,scan_ff60},{59,scan_ff59}
  }};
  for(const auto& [major, scan]:versions) {
    ModuleHandle format=retain_loaded_module("avformat",major),codec{};
    if(!format)continue;
    codec=retain_loaded_module("avcodec",major);
    if(!codec){release_module(format);continue;}
    struct Modules {ModuleHandle f,c;~Modules(){release_module(c);release_module(f);}} modules{format,codec};
    const auto begin=monotonic_ms();
    const bool ok=scan(path,format,codec,metadata);
    log("metadata scan ok=%d ABI=%d duration=%.9f frames=%lld packets=%lld rate=%d channels=%d elapsed_ms=%llu error=%s file=%s",ok,major,metadata.duration,metadata.frames,metadata.packets,metadata.rate,metadata.channels,static_cast<unsigned long long>(monotonic_ms()-begin),metadata.error.c_str(),path);
    if(ok && file_key(path)==key){cache[key]=metadata;return true;}
    if(ok)metadata.error="Source changed during metadata scan";
    return false;
  }
  metadata.error=no_loaded_ffmpeg;return false;
}
}

#include "thd_ac3.h"
#include <array>
namespace frzz {
std::unique_ptr<ThdAc3Decoder> create_thd_ac3_decoder(const char* path,std::string& error) {
  struct Version {int codec,util;ThdFactory factory;};
  const std::array<Version,4> versions{{{62,60,create_thd_ff62},{61,59,create_thd_ff61},{60,58,create_thd_ff60},{59,57,create_thd_ff59}}};
  for(const auto& version:versions) {
    ModuleHandle codec=retain_loaded_module("avcodec",version.codec),util{};
    if(!codec)continue;
    util=retain_loaded_module("avutil",version.util);
    if(!util){release_module(codec);continue;}
    auto result=std::unique_ptr<ThdAc3Decoder>(version.factory(path,codec,util,error));
    if(!result){release_module(util);release_module(codec);}
    return result;
  }
  error="No loaded FFmpeg 5.x-8.x codec libraries available";
  return {};
}
}

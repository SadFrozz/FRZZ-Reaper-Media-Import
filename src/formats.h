#pragma once
#include <array>
#include <string_view>
namespace frzz {
enum class Format { unsupported, raw_video, ass, ssa, srt };
struct Extension { const char* name; Format format; };
inline constexpr std::array<Extension,25> extensions_list{{
  {"ac3",Format::raw_video},{"eac3",Format::raw_video},{"ec3",Format::raw_video},
  {"ddp",Format::raw_video},{"ac4",Format::raw_video},{"thd",Format::raw_video},
  {"truehd",Format::raw_video},{"thd+ac3",Format::raw_video},{"mlp",Format::raw_video},
  {"dts",Format::raw_video},{"dtshd",Format::raw_video},{"dts-hd",Format::raw_video},
  {"dtsma",Format::raw_video},{"dts-ma",Format::raw_video},{"dts-hdma",Format::raw_video},
  {"dts-hd-ma",Format::raw_video},{"dtses",Format::raw_video},{"dts-es",Format::raw_video},
  {"dtsx",Format::raw_video},{"dts-x",Format::raw_video},{"dtsuhd",Format::raw_video},
  {"dts-uhd",Format::raw_video},{"ass",Format::ass},{"ssa",Format::ssa},{"srt",Format::srt}
}};
inline Format identify_format(std::string_view path) {
  const auto dot=path.rfind('.');
  if(dot==std::string_view::npos) return Format::unsupported;
  const auto ext=path.substr(dot+1);
  for(auto& e:extensions_list) {
    const std::string_view name(e.name); if(ext.size()!=name.size()) continue;
    bool match=true;
    for(size_t i=0;i<ext.size();++i) {char c=ext[i];if(c>='A' && c<='Z')c+=('a'-'A');if(c!=name[i]){match=false;break;}}
    if(match) return e.format;
  }
  return Format::unsupported;
}
}

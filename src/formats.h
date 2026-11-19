#pragma once
#include <array>
#include <string_view>
namespace frzz {
enum class Format { unsupported, raw_audio, ass, ssa, srt };
struct Extension { const char* name; Format format; };
inline constexpr std::array<Extension,25> extensions_list{{
  {"ac3",Format::raw_audio},{"eac3",Format::raw_audio},{"ec3",Format::raw_audio},
  {"ddp",Format::raw_audio},{"ac4",Format::raw_audio},{"thd",Format::raw_audio},
  {"truehd",Format::raw_audio},{"thd+ac3",Format::raw_audio},{"mlp",Format::raw_audio},
  {"dts",Format::raw_audio},{"dtshd",Format::raw_audio},{"dts-hd",Format::raw_audio},
  {"dtsma",Format::raw_audio},{"dts-ma",Format::raw_audio},{"dts-hdma",Format::raw_audio},
  {"dts-hd-ma",Format::raw_audio},{"dtses",Format::raw_audio},{"dts-es",Format::raw_audio},
  {"dtsx",Format::raw_audio},{"dts-x",Format::raw_audio},{"dtsuhd",Format::raw_audio},
  {"dts-uhd",Format::raw_audio},{"ass",Format::ass},{"ssa",Format::ssa},{"srt",Format::srt}
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

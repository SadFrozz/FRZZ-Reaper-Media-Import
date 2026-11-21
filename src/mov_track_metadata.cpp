#include "mov_track_metadata.h"
#include "container_decoder.h"
#include "text_encoding.h"
#include <array>
#include <fstream>
#include <optional>
#include <string_view>

namespace frzz {
namespace {
constexpr std::uint32_t tag(unsigned a,unsigned b,unsigned c,unsigned d) {
  return (a<<24)|(b<<16)|(c<<8)|d;
}
constexpr auto title_tag=tag(0xa9,'n','a','m');
constexpr std::uint32_t tag(const char (&s)[5]) {return tag(s[0],s[1],s[2],s[3]);}

std::uint64_t be(std::string_view bytes) {
  std::uint64_t value=0;
  for(const unsigned char b:bytes)value=(value<<8)|b;
  return value;
}

struct Box {
  std::uint32_t type{};
  std::uint64_t data{},end{};
};

struct Reader {
  std::ifstream file;
  unsigned boxes_left=50000;
  explicit Reader(const char* path):file(std::filesystem::u8path(path),std::ios::binary){}

  bool read(std::uint64_t offset,char* out,size_t count) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    return static_cast<bool>(file.read(out,static_cast<std::streamsize>(count)));
  }
  std::optional<Box> box(std::uint64_t& at,std::uint64_t end) {
    if(at>end || end-at<8 || !boxes_left)return {};
    --boxes_left;
    char raw[8];
    if(!read(at,raw,sizeof(raw)))return {};
    const auto type=static_cast<std::uint32_t>(be(std::string_view(raw+4,4)));
    auto size=be(std::string_view(raw,4));
    std::uint64_t header=8;
    if(size==1) {
      if(end-at<16 || !read(at+8,raw,sizeof(raw)))return {};
      size=be(std::string_view(raw,8));
      header=16;
    } else if(!size)size=end-at;
    if(size<header || size>end-at)return {};
    const Box result{type,at+header,at+size};
    at=result.end;
    return result;
  }
  std::optional<Box> child(const Box& parent,std::uint32_t type) {
    auto at=parent.data;
    while(auto item=box(at,parent.end))if(item->type==type)return item;
    return {};
  }
  std::string payload(const Box& item,std::uint64_t skip=0) {
    const auto size=item.end-item.data;
    if(size<=skip || size-skip>65536)return {};
    std::string bytes(static_cast<size_t>(size-skip),'\0');
    if(!read(item.data+skip,bytes.data(),bytes.size()))return {};
    return bytes;
  }
};

std::string text(std::string_view bytes,bool utf16=false) {
  try {
    const bool little=bytes.substr(0,2)=="\xff\xfe";
    const bool bom=little || bytes.substr(0,2)=="\xfe\xff";
    if(utf16 || bom) {
      if(bom)bytes.remove_prefix(2);
      for(size_t at=0;at+1<bytes.size();at+=2) {
        if(bytes[at]=='\0' && bytes[at+1]=='\0'){bytes=bytes.substr(0,at);break;}
      }
      return decode_utf16(bytes,little);
    }
    bytes=bytes.substr(0,bytes.find('\0'));
    if(bytes.substr(0,3)=="\xef\xbb\xbf")bytes.remove_prefix(3);
    return valid_utf8(bytes)?std::string(bytes):std::string{};
  }catch(const std::runtime_error&){return {};}
}

std::string packed_language(std::uint64_t code) {
  std::string result;
  if(code>0x7fff)return result;
  for(unsigned shift:{10,5,0}) {
    const auto part=(code>>shift)&31;
    if(part<1 || part>26)return {};
    result+=static_cast<char>('a'+part-1);
  }
  return result;
}

std::uint32_t track_id(Reader& reader,const Box& header) {
  char data[24]{};
  if(header.end-header.data<16 || !reader.read(header.data,data,16))return 0;
  if(data[0]==0)return static_cast<std::uint32_t>(be(std::string_view(data+12,4)));
  if(data[0]==1 && header.end-header.data>=24 && reader.read(header.data+20,data,4))
    return static_cast<std::uint32_t>(be(std::string_view(data,4)));
  return 0;
}

std::string extended_language(Reader& reader,const Box& media) {
  const auto field=reader.child(media,tag("elng"));
  if(!field)return {};
  const auto label=text(reader.payload(*field,4));
  if(label.empty() || label.size()>127 || label.front()=='-' || label.back()=='-')return {};
  for(const auto c:label)
    if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-'))return {};
  return label;
}

std::uint32_t title_key(Reader& reader,const Box& keys) {
  const auto data=reader.payload(keys);
  if(data.size()<8)return 0;
  const auto count=be(std::string_view(data).substr(4,4));
  std::string_view entries(data.data()+8,data.size()-8);
  for(std::uint32_t i=1;i<=count && entries.size()>=8;++i) {
    const auto size=be(entries.substr(0,4));
    if(size<8 || size>entries.size())break;
    if(entries.substr(4,4)=="mdta" && entries.substr(8,static_cast<size_t>(size)-8)=="com.apple.quicktime.title")return i;
    entries.remove_prefix(static_cast<size_t>(size));
  }
  return 0;
}

std::string metadata_title(Reader& reader,const Box& parent) {
  auto meta=reader.child(parent,tag("meta"));
  if(!meta)return {};
  auto list=reader.child(*meta,tag("ilst"));
  // ISO BMFF meta is a FullBox; QuickTime also uses meta without version/flags.
  if(!list && meta->end-meta->data>=4) {
    meta->data+=4;
    list=reader.child(*meta,tag("ilst"));
  }
  if(!list)return {};
  const auto keys=reader.child(*meta,tag("keys"));
  const auto key=keys?title_key(reader,*keys):0;
  auto at=list->data;
  while(auto item=reader.box(at,list->end)) {
    if(item->type!=title_tag && (!key || item->type!=key))continue;
    auto value_at=item->data;
    while(auto value=reader.box(value_at,item->end)) {
      if(value->type!=tag("data"))continue;
      const auto bytes=reader.payload(*value);
      if(bytes.size()<9)continue;
      const auto type=be(std::string_view(bytes).substr(0,4));
      if(type!=1 && type!=2)continue;
      auto label=text(std::string_view(bytes).substr(8),type==2);
      if(!label.empty())return label;
    }
  }
  return {};
}

std::string user_data_title(Reader& reader,const Box& user,std::string_view language) {
  std::string localized,legacy;
  auto at=user.data;
  while(auto item=reader.box(at,user.end)) {
    if(item->type==tag("tnam")) {
      const auto bytes=reader.payload(*item);
      if(bytes.size()<=6)continue;
      std::string_view data(bytes);
      auto label=text(data.substr(6));
      if(label.empty())continue;
      const auto code=packed_language(be(data.substr(4,2)));
      if(!code.empty() && code==language)return label;
      if(localized.empty())localized=std::move(label);
    } else if(item->type==tag("name") && legacy.empty()) {
      // The FFmpeg MOV/MP4 muxer writes a raw UTF-8 name in trak/udta.
      legacy=text(reader.payload(*item));
    } else if(item->type==title_tag && legacy.empty()) {
      const auto bytes=reader.payload(*item);
      if(bytes.size()<4)continue;
      const auto length=be(std::string_view(bytes).substr(0,2));
      if(length<=bytes.size()-4)legacy=text(std::string_view(bytes).substr(4,static_cast<size_t>(length)));
    }
  }
  return localized.empty()?legacy:localized;
}
}

void read_mov_track_metadata(const char* path,ContainerAudio& audio) {
  Reader reader(path);
  if(!reader.file)return;
  reader.file.seekg(0,std::ios::end);
  const auto size=reader.file.tellg();
  if(size<0)return;
  const auto movie=reader.child({0,0,static_cast<std::uint64_t>(size)},tag("moov"));
  if(!movie)return;
  auto at=movie->data;
  while(auto track=reader.box(at,movie->end)) {
    if(track->type!=tag("trak"))continue;
    const auto header=reader.child(*track,tag("tkhd"));
    const auto id=header?track_id(reader,*header):0;
    if(!id)continue;
    for(auto& stream:audio.streams) {
      if(stream.track_id!=id)continue;
      const auto media=reader.child(*track,tag("mdia"));
      const auto user=reader.child(*track,tag("udta"));
      const auto language=media?extended_language(reader,*media):std::string{};
      auto title=metadata_title(reader,*track);
      if(title.empty() && media)title=metadata_title(reader,*media);
      if(title.empty() && user)title=metadata_title(reader,*user);
      if(title.empty() && user)title=user_data_title(reader,*user,stream.language);
      if(!title.empty())stream.title=std::move(title);
      if(!language.empty())stream.language=language;
      break;
    }
  }
}
}

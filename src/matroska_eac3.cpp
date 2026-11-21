#include "matroska_eac3.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace frzz {
namespace {
using Offset=std::uint64_t;
constexpr Offset unknown_size=~Offset{0};
constexpr Offset ebml=0x1a45dfa3,segment=0x18538067,tracks_id=0x1654ae6b,cluster_id=0x1f43b675;
constexpr Offset track_entry=0xae,track_number=0xd7,track_type=0x83,codec_id=0x86,encodings=0x6d80,track_clock=0x23314f;
constexpr Offset simple_block=0xa3,block_group=0xa0,block=0xa1;

class File {
  std::ifstream file;
  std::array<unsigned char,4096> bytes{};
  Offset cached=unknown_size;
  std::size_t loaded{};
public:
  Offset size{};
  explicit File(const char* path):file(std::filesystem::u8path(path),std::ios::binary|std::ios::ate) {
    const auto end=file.tellg();
    if(end>0)size=static_cast<Offset>(end);
  }
  bool read(Offset at,void* destination,std::size_t count) {
    if(at>size || count>size-at)return false;
    auto* out=static_cast<unsigned char*>(destination);
    while(count) {
      if(cached==unknown_size || at<cached || at-cached>=loaded) {
        cached=at-at%bytes.size();
        file.clear();
        file.seekg(static_cast<std::streamoff>(cached));
        loaded=static_cast<std::size_t>(std::min<Offset>(bytes.size(),size-cached));
        if(!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(loaded)))return false;
      }
      const auto start=static_cast<std::size_t>(at-cached);
      const auto part=std::min(count,loaded-start);
      std::memcpy(out,bytes.data()+start,part);
      at+=part;
      out+=part;
      count-=part;
    }
    return true;
  }
};

bool read_vint(File& file,Offset& at,Offset end,Offset& value,bool is_id=false) {
  unsigned char bytes[8];
  if(at>=end || !file.read(at,bytes,1) || !bytes[0])return false;
  unsigned mask=128,width=1;
  while(!(bytes[0]&mask)){mask>>=1;++width;}
  if((is_id && width>4) || width>end-at || !file.read(at,bytes,width))return false;
  value=is_id?bytes[0]:bytes[0]&(mask-1);
  for(unsigned i=1;i<width;++i)value=(value<<8)|bytes[i];
  if(!is_id && value==((Offset{1}<<(7*width))-1))value=unknown_size;
  at+=width;
  return true;
}

struct Element {
  Offset id{},begin{},end{};
};
bool next_element(File& file,Offset& at,Offset limit,Element& out,bool allow_segment_size=false) {
  Offset length{};
  if(!read_vint(file,at,limit,out.id,true) || !read_vint(file,at,limit,length))return false;
  if(length==unknown_size && allow_segment_size && out.id==segment)length=limit-at;
  if(length>limit-at)return false;
  out.begin=at;
  at+=length;
  out.end=at;
  return true;
}

struct Eac3Track {
  Offset number{};
  bool seen=false;
};
bool read_uint(File& file,const Element& element,Offset& result) {
  const auto length=element.end-element.begin;
  unsigned char bytes[8];
  if(!length || length>sizeof(bytes) || !file.read(element.begin,bytes,static_cast<std::size_t>(length)))return false;
  result=0;
  for(Offset i=0;i<length;++i)result=(result<<8)|bytes[i];
  return true;
}

bool read_tracks(File& file,const Element& element,std::vector<Eac3Track>& eac3) {
  std::vector<Offset> numbers;
  for(Offset at=element.begin;at<element.end;) {
    Element entry;
    if(!next_element(file,at,element.end,entry))return false;
    if(entry.id!=track_entry)continue;
    Offset number{},type{};
    bool matches=false,transformed=false,codec_seen=false;
    for(Offset field=entry.begin;field<entry.end;) {
      Element value;
      if(!next_element(file,field,entry.end,value))return false;
      const auto length=value.end-value.begin;
      if(value.id==track_number) {
        if(number || !read_uint(file,value,number) || !number)return false;
      } else if(value.id==track_type) {
        if(type || !read_uint(file,value,type) || !type)return false;
      } else if(value.id==codec_id) {
        if(codec_seen)return false;
        codec_seen=true;
        if(length==6) {
          char raw[6];
          if(!file.read(value.begin,raw,sizeof(raw)))return false;
          matches=std::memcmp(raw,"A_EAC3",sizeof(raw))==0;
        }
      } else if(value.id==encodings || value.id==track_clock)transformed=true;
    }
    if(!number || !type || !codec_seen || std::find(numbers.begin(),numbers.end(),number)!=numbers.end())return false;
    numbers.push_back(number);
    if(matches) {
      if(type!=2 || transformed)return false;
      eac3.push_back({number});
    }
  }
  return !eac3.empty();
}

bool valid_eac3_packet(File& file,Offset at,Offset end,int num,int den) {
  constexpr int rates[]={48000,44100,32000};
  constexpr int blocks[]={1,2,3,6};
  while(at<end) {
    unsigned char h[6];
    if(end-at<sizeof(h) || !file.read(at,h,sizeof(h)) || h[0]!=0x0b || h[1]!=0x77)return false;
    const unsigned bsid=h[5]>>3;
    if(bsid<=10 || bsid>16)return false;
    const unsigned code=h[4]>>6, sub=(h[4]>>4)&3;
    if(code==3 && sub==3)return false;
    const int rate=code==3?rates[sub]/2:rates[code];
    const int samples=256*(code==3?6:blocks[sub]);
    const auto length=(1u+((h[2]&7u)<<8)+h[3])*2;
    if(length<sizeof(h) || length>end-at || (std::int64_t(samples)*den)%(std::int64_t(rate)*num)!=0)return false;
    at+=length;
  }
  return true;
}

bool verify_block(File& file,const Element& element,std::vector<Eac3Track>& tracks,int num,int den,bool grouped) {
  Offset number{},payload=element.begin;
  if(!read_vint(file,payload,element.end,number))return false;
  const auto track=std::find_if(tracks.begin(),tracks.end(),[&](const Eac3Track& row){return row.number==number;});
  if(track==tracks.end())return true;
  // BlockGroups may trim a packet. Leave them to normal decoding.
  unsigned char header[3];
  if(grouped || element.end-payload<sizeof(header) || !file.read(payload,header,sizeof(header)))return false;
  payload+=sizeof(header);
  unsigned count=1;
  const auto lacing=header[2]&6u;
  if(lacing==4) {
    unsigned char lace{};
    if(payload==element.end || !file.read(payload++,&lace,1))return false;
    count+=lace;
  } else if(lacing)return false;
  const auto length=(element.end-payload)/count;
  if(length<6 || length*count!=element.end-payload)return false;
  for(unsigned i=0;i<count;++i,payload+=length)
    if(!valid_eac3_packet(file,payload,payload+length,num,den))return false;
  track->seen=true;
  return true;
}

bool verify_cluster(File& file,const Element& cluster,std::vector<Eac3Track>& tracks,int num,int den) {
  for(Offset at=cluster.begin;at<cluster.end;) {
    Element child;
    if(!next_element(file,at,cluster.end,child))return false;
    if(child.id==simple_block && !verify_block(file,child,tracks,num,den,false))return false;
    if(child.id!=block_group)continue;
    for(Offset sub=child.begin;sub<child.end;) {
      Element part;
      if(!next_element(file,sub,child.end,part))return false;
      if(part.id==block && !verify_block(file,part,tracks,num,den,true))return false;
    }
  }
  return true;
}
}

bool matroska_eac3_on_timestamp_grid(const char* path,int tick_num,int tick_den,unsigned expected_tracks) {
  if(tick_num<=0 || tick_den<=0 || !expected_tracks)return false;
  try {
    File file(path);
    Offset at=0;
    Element head,media;
    if(!next_element(file,at,file.size,head) || head.id!=ebml ||
       !next_element(file,at,file.size,media,true) || media.id!=segment || media.end!=file.size)return false;
    std::vector<Eac3Track> audio;
    for(at=media.begin;at<media.end;) {
      Element element;
      if(!next_element(file,at,media.end,element))return false;
      if(element.id==tracks_id && (!audio.empty() || !read_tracks(file,element,audio) ||
         audio.size()!=expected_tracks))return false;
      if(element.id==cluster_id && (audio.empty() || !verify_cluster(file,element,audio,tick_num,tick_den)))return false;
    }
    return !audio.empty() && std::all_of(audio.begin(),audio.end(),[](const Eac3Track& track){return track.seen;});
  } catch(...) {return false;}
}
}

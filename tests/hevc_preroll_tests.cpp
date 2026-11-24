#include "hevc_preroll.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <vector>

using frzz::hevc_preroll::Picture;

namespace {
using Bytes=std::vector<uint8_t>;

void check(bool result,const char* name) {
  if(!result) {
    std::fprintf(stderr,"%s\n",name);
    std::exit(1);
  }
}

void append_nal(Bytes& packet,std::initializer_list<uint8_t> nal,unsigned length_size) {
  packet.insert(packet.end(),length_size-1,0);
  packet.push_back(static_cast<uint8_t>(nal.size()));
  packet.insert(packet.end(),nal);
}

Bytes configuration(uint8_t vps_layers=0,uint8_t sps_layers=0) {
  Bytes bytes(23);
  bytes[0]=1;
  bytes[21]=0x0b;
  bytes[22]=3;
  bytes.insert(bytes.end(),{32,0,1});
  append_nal(bytes,{64,1,0,vps_layers},2);
  bytes.insert(bytes.end(),{33,0,1});
  append_nal(bytes,{66,1,sps_layers},2);
  bytes.insert(bytes.end(),{34,0,1});
  append_nal(bytes,{68,1,0x80},2);
  return bytes;
}

unsigned packet_length_size(const Bytes& config) {
  return frzz::hevc_preroll::single_layer_length_size(config.data(),config.size());
}

auto access_unit(const Bytes& packet,const Bytes& config={}) {
  return frzz::hevc_preroll::classify(packet.data(),packet.size(),4,config.data(),config.size());
}

Picture picture(const Bytes& packet) {
  return access_unit(packet).picture;
}
}

int main() {
  using frzz::hevc_preroll::ends_before_or_at;
  check(!ends_before_or_at(967,2033,1778),"A long VFR picture still covers the seek boundary");
  check(ends_before_or_at(1000,40,1040),"A picture ending at the boundary can be discarded");
  check(!ends_before_or_at(1000,0,1040),"A picture with unknown duration is retained");
  check(!ends_before_or_at(1040,40,1040),"A picture at the boundary is retained");
  const auto max=std::numeric_limits<int64_t>::max();
  check(!ends_before_or_at(max-10,40,max),"A picture extending beyond the timestamp range is retained");
  check(ends_before_or_at(-max,max,max),"Very early timestamps can be safely compared");

  auto config=configuration();
  check(packet_length_size(config)==4,"Single-layer hvcC is accepted");
  config[21]=0x13;
  check(packet_length_size(config)==0,"Multiple declared temporal layers are excluded");
  check(packet_length_size(configuration(2,0))==0,"Multiple temporal layers in VPS are excluded");
  check(packet_length_size(configuration(16,0))==0,"Multiple spatial layers in VPS are excluded");
  check(packet_length_size(configuration(0,2))==0,"Multiple temporal layers in SPS are excluded");
  config=configuration();
  config.pop_back();
  check(packet_length_size(config)==0,"Truncated parameter sets are excluded");

  Bytes packet;
  append_nal(packet,{38,1,0xa0},4);
  check(picture(packet)==Picture::idr,"Single-layer IDR is identified");
  check(!access_unit(packet,config).current_parameters,"An IDR can retain changed parameter sets across a seek");
  packet.clear();
  const std::initializer_list<uint8_t> video{64,1,0,1};
  const std::initializer_list<uint8_t> sequence{66,1,1,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x80};
  const std::initializer_list<uint8_t> parameters{68,1,0xc0};
  config.resize(23);
  config.insert(config.end(),{32,0,1});
  append_nal(config,video,2);
  config.insert(config.end(),{33,0,1});
  append_nal(config,sequence,2);
  config.insert(config.end(),{34,0,1});
  append_nal(config,parameters,2);
  append_nal(packet,video,4);
  append_nal(packet,sequence,4);
  append_nal(packet,parameters,4);
  append_nal(packet,{38,1,0xa0},4);
  check(access_unit(packet,config).current_parameters,"An IDR with its current VPS/SPS/PPS chain is accepted");
  config.back()=0xd0;
  check(!access_unit(packet,config).current_parameters,"Different contents with the same parameter ID are rejected");
  config.back()=0xc0;
  packet.back()=0x90;
  check(!access_unit(packet,config).current_parameters,"Unrelated in-band parameter sets are insufficient");
  packet.clear();
  append_nal(packet,{42,1,0xa0},4);
  check(access_unit(packet).irap,"CRA must terminate an earlier IDR's proof");
  packet.clear();
  append_nal(packet,{0,1,0x80},4);
  check(picture(packet)==Picture::nonreference,"TRAIL_N picture can be considered");
  packet[5]=2;
  check(picture(packet)==Picture::unsuitable,"A higher temporal layer is excluded");
  packet[5]=1;
  packet[6]=0;
  check(picture(packet)==Picture::unsuitable,"A continuation slice is excluded");
  packet.clear();
  append_nal(packet,{2,1,0x80},4);
  check(picture(packet)==Picture::keep,"TRAIL_R must be decoded");

  packet.clear();
  append_nal(packet,{78,1,4,8,0x26,0,4,0,5,0,0,3,2,0x80},4);
  check(picture(packet)==Picture::unsuitable,"A standalone SEI packet is not a complete access unit");
  append_nal(packet,{0,1,0x80},4);
  check(picture(packet)==Picture::nonreference,"One HDR Vivid SEI message can stay in the decoder");
  packet.clear();
  append_nal(packet,{78,1,4,6,0x26,0,4,0,5,1,5,1,0,0x80},4);
  append_nal(packet,{0,1,0x80},4);
  check(picture(packet)==Picture::keep,"Additional SEI messages prevent discarding");
}

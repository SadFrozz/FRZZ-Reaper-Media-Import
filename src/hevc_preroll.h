#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace frzz::hevc_preroll {

inline bool ends_before_or_at(int64_t pts,int64_t duration,int64_t cutoff) noexcept {
  if(duration<=0 || pts>=cutoff)return false;
  // Subtract in unsigned arithmetic to cover the full range of timestamp gaps.
  return uint64_t(duration)<=uint64_t(cutoff)-uint64_t(pts);
}

struct Nal {
  const uint8_t* data;
  std::size_t size;

  unsigned type() const noexcept {return (data[0]>>1)&63;}
  bool base_layer() const noexcept {
    return size>=3 && !(data[0]&0x81) && data[1]==1;
  }
  bool parameters_are_single_layer() const noexcept {
    if(type()==32)return size>=4 && !(data[2]&3) && !(data[3]&0xfe);
    if(type()==33)return !(data[2]&0x0e);
    return true;
  }
};

inline Nal read_nal(const uint8_t*& bytes,std::size_t& remaining,unsigned length_size) noexcept {
  if(remaining<length_size)return {};
  std::size_t length=0;
  for(unsigned i=0;i<length_size;++i)length=(length<<8)|bytes[i];
  if(length<3 || length>remaining-length_size)return {};
  const Nal nal{bytes+length_size,length};
  bytes+=length_size+length;
  remaining-=length_size+length;
  return nal;
}

inline unsigned single_layer_length_size(const uint8_t* bytes,std::size_t remaining) noexcept {
  // HEVCDecoderConfigurationRecord followed by the NAL arrays.
  if(!bytes || remaining<23 || bytes[0]!=1 || ((bytes[21]>>3)&7)!=1)return 0;
  const unsigned length_size=(bytes[21]&3)+1;
  const unsigned arrays=bytes[22];
  bytes+=23;
  remaining-=23;
  bool vps=false,sps=false,pps=false;
  for(unsigned i=0;i<arrays;++i) {
    if(remaining<3)return 0;
    const unsigned type=bytes[0]&63;
    const unsigned count=(unsigned(bytes[1])<<8)|bytes[2];
    bytes+=3;
    remaining-=3;
    for(unsigned n=0;n<count;++n) {
      const auto nal=read_nal(bytes,remaining,2);
      if(!nal.base_layer() || nal.type()!=type || !nal.parameters_are_single_layer())return 0;
      vps|=type==32;
      sps|=type==33;
      pps|=type==34;
    }
  }
  return !remaining && vps && sps && pps ? length_size : 0;
}

inline bool is_vivid_sei(Nal nal) noexcept {
  // Accept exactly one registered HDR Vivid message. Other SEI can be
  // consumed on the next decoded frame if its own picture is skipped.
  std::array<uint8_t,258> rbsp{};
  std::size_t length=0;
  unsigned zeros=0;
  for(std::size_t i=2;i<nal.size;++i) {
    const auto byte=nal.data[i];
    if(zeros==2 && byte==3) {
      if(i+1==nal.size || nal.data[i+1]>3)return false;
      zeros=0;
      continue;
    }
    if(zeros==2 && byte<3)return false;
    if(length==rbsp.size())return false;
    rbsp[length++]=byte;
    zeros=byte==0?zeros+1:0;
  }
  return length>=9 && rbsp[0]==4 && rbsp[1]>=6 && rbsp[1]!=255 &&
    length==std::size_t(rbsp[1])+3 && rbsp[length-1]==0x80 &&
    rbsp[2]==0x26 && rbsp[3]==0 && rbsp[4]==4 && rbsp[5]==0 && rbsp[6]==5;
}

enum class Picture { unsuitable, keep, idr, nonreference };

struct AccessUnit {
  Picture picture=Picture::unsuitable;
  bool parameters=false;
  bool current_parameters=false;
  bool irap=false;
};

class RbspBits {
public:
  explicit RbspBits(Nal nal) noexcept : next_(nal.data+2), left_(nal.size-2) {}

  int bit() noexcept {
    if(!bits_) {
      if(!left_)return -1;
      byte_=*next_++;
      --left_;
      if(zeros_==2 && byte_==3) {
        if(!left_ || *next_>3)return -1;
        byte_=*next_++;
        --left_;
        zeros_=0;
      } else if(zeros_==2 && byte_<3) {
        return -1;
      }
      zeros_=byte_==0?zeros_+1:0;
      bits_=8;
    }
    return (byte_>>--bits_)&1;
  }

  bool skip(unsigned count) noexcept {
    while(count--)if(bit()<0)return false;
    return true;
  }

  int unsigned_exp_golomb() noexcept {
    unsigned zeros=0;
    int part;
    while((part=bit())==0)if(++zeros>6)return -1;
    if(part<0)return -1;
    int value=1;
    while(zeros--) {
      part=bit();
      if(part<0)return -1;
      value=(value<<1)|part;
    }
    return value-1;
  }

private:
  const uint8_t* next_;
  std::size_t left_;
  unsigned byte_=0,bits_=0,zeros_=0;
};

inline bool matching_parameters(const uint8_t* config,std::size_t remaining,
                                const std::array<Nal,3>& chain) noexcept {
  if(!config || remaining<23)return false;
  const unsigned arrays=config[22];
  config+=23;
  remaining-=23;
  unsigned found=0;
  for(unsigned i=0;i<arrays;++i) {
    if(remaining<3)return false;
    const unsigned count=(unsigned(config[1])<<8)|config[2];
    config+=3;
    remaining-=3;
    for(unsigned n=0;n<count;++n) {
      const auto nal=read_nal(config,remaining,2);
      if(!nal.data)return false;
      for(unsigned part=0;part<chain.size();++part)
        if(nal.size==chain[part].size && !std::memcmp(nal.data,chain[part].data,nal.size))
          found|=1u<<part;
    }
  }
  return found==7;
}

inline bool idr_has_current_parameters(Nal idr,const uint8_t* bytes,std::size_t remaining,unsigned length_size,
                                       const uint8_t* config,std::size_t config_size) noexcept {
  // An IDR resets references but can reuse parameter sets retained by the
  // decoder across a seek. Verify the IDR's actual PPS -> SPS -> VPS chain.
  RbspBits slice(idr);
  if(!slice.skip(2))return false;
  const int selected_pps=slice.unsigned_exp_golomb();
  if(selected_pps<0 || selected_pps>=64)return false;

  struct Parameter {
    Nal nal;
    int parent;
  };
  std::array<Parameter,64> pps{};
  std::array<Parameter,16> sps{};
  std::array<Nal,16> vps{};
  while(remaining) {
    const auto nal=read_nal(bytes,remaining,length_size);
    if(!nal.base_layer() || !nal.parameters_are_single_layer())return false;
    if(nal.data==idr.data)break;
    if(nal.type()==32) {
      vps[nal.data[2]>>4]=nal;
    } else if(nal.type()==33) {
      RbspBits sequence(nal);
      // The one-layer profile_tier_level contains 96 bits.
      if(!sequence.skip(8+96))return false;
      const int id=sequence.unsigned_exp_golomb();
      if(id<0 || id>=16)return false;
      sps[id]={nal,nal.data[2]>>4};
    } else if(nal.type()==34) {
      RbspBits picture(nal);
      const int id=picture.unsigned_exp_golomb();
      const int sequence=picture.unsigned_exp_golomb();
      if(id<0 || id>=64 || sequence<0 || sequence>=16)return false;
      pps[id]={nal,sequence};
    }
  }
  const auto& picture=pps[selected_pps];
  const auto& sequence=sps[picture.parent];
  const auto& video=vps[sequence.parent];
  return picture.nal.data && sequence.nal.data && video.data &&
    video.data<sequence.nal.data && sequence.nal.data<picture.nal.data &&
    matching_parameters(config,config_size,{video,sequence.nal,picture.nal});
}

inline AccessUnit classify(const uint8_t* bytes,std::size_t remaining,unsigned length_size,
                           const uint8_t* config=nullptr,std::size_t config_size=0) noexcept {
  if(!bytes || !remaining || !length_size || length_size>4)return {};
  const auto* start=bytes;
  const auto size=remaining;
  bool discardable=true;
  AccessUnit result{Picture::keep,false};
  bool have_picture=false;
  while(remaining) {
    const auto nal=read_nal(bytes,remaining,length_size);
    if(!nal.base_layer() || !nal.parameters_are_single_layer())return {};
    const auto type=nal.type();
    if(type==36 || type==37)return {};
    result.parameters|=type>=32 && type<=34;
    if(type<32) {
      if(have_picture || !(nal.data[2]&128))return {};
      have_picture=true;
      result.irap=type>=16 && type<=23;
      if(type==19 || type==20) {
        result.picture=Picture::idr;
        if(config)result.current_parameters=idr_has_current_parameters(nal,start,size,length_size,config,config_size);
      }
      if(type==0 && discardable && !remaining)result.picture=Picture::nonreference;
    } else if(type!=35 && (type!=39 || !is_vivid_sei(nal))) {
      discardable=false;
    }
  }
  return have_picture?result:AccessUnit{};
}

}

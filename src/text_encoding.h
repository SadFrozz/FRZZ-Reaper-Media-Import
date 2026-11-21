#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace frzz {
inline bool valid_utf8(std::string_view data) {
  for(size_t i=0;i<data.size();) {
    const auto c=static_cast<unsigned char>(data[i]);
    if(c<0x80){++i;continue;}
    int extra=0;unsigned int value=0,minimum=0;
    if((c&0xe0)==0xc0){extra=1;value=c&0x1f;minimum=0x80;}
    else if((c&0xf0)==0xe0){extra=2;value=c&0x0f;minimum=0x800;}
    else if((c&0xf8)==0xf0){extra=3;value=c&0x07;minimum=0x10000;}
    else return false;
    if(i+static_cast<size_t>(extra)>=data.size())return false;
    for(int n=0;n<extra;++n){const auto next=static_cast<unsigned char>(data[++i]);if((next&0xc0)!=0x80)return false;value=(value<<6)|(next&0x3f);}
    if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;
    ++i;
  }
  return true;
}
inline void append_utf8(std::string& out,unsigned codepoint) {
  if(codepoint<0x80) {
    out+=static_cast<char>(codepoint);
    return;
  }
  if(codepoint<0x800)out+=static_cast<char>(0xc0|(codepoint>>6));
  else {
    if(codepoint<0x10000)out+=static_cast<char>(0xe0|(codepoint>>12));
    else {
      out+=static_cast<char>(0xf0|(codepoint>>18));
      out+=static_cast<char>(0x80|((codepoint>>12)&0x3f));
    }
    out+=static_cast<char>(0x80|((codepoint>>6)&0x3f));
  }
  out+=static_cast<char>(0x80|(codepoint&0x3f));
}

inline std::string decode_utf16(std::string_view bytes,bool little_endian) {
  if(bytes.size()%2!=0)throw std::runtime_error("Truncated UTF-16 text");
  auto read_unit=[&](size_t at)->unsigned {
    const auto a=static_cast<unsigned char>(bytes[at]);
    const auto b=static_cast<unsigned char>(bytes[at+1]);
    return little_endian ? (b<<8)|a : (a<<8)|b;
  };
  std::string utf8;
  utf8.reserve(bytes.size());
  for(size_t at=0;at<bytes.size();at+=2) {
    auto codepoint=read_unit(at);
    if(codepoint>=0xd800 && codepoint<=0xdbff) {
      if(bytes.size()-at<4)throw std::runtime_error("Truncated UTF-16 surrogate pair");
      const auto low=read_unit(at+2);
      if(low<0xdc00 || low>0xdfff)throw std::runtime_error("Invalid UTF-16 surrogate pair");
      codepoint=0x10000+((codepoint-0xd800)<<10)+(low-0xdc00);
      at+=2;
    } else if(codepoint==0 || (codepoint>=0xdc00 && codepoint<=0xdfff)) {
      throw std::runtime_error("Invalid UTF-16 character");
    }
    append_utf8(utf8,codepoint);
  }
  return utf8;
}

}

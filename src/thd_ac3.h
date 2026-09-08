#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "platform.h"
namespace frzz {
class ThdAc3Decoder {
public:
  virtual ~ThdAc3Decoder()=default;
  virtual int rate() const=0;
  virtual int channels() const=0;
  virtual std::int64_t frames() const=0;
  virtual int abi() const=0;
  virtual int read(std::int64_t position,double* output,int length)=0;
};
using ThdFactory=ThdAc3Decoder*(*)(const char*,ModuleHandle,ModuleHandle,std::string&);
ThdAc3Decoder* create_thd_ff59(const char*,ModuleHandle,ModuleHandle,std::string&);
ThdAc3Decoder* create_thd_ff60(const char*,ModuleHandle,ModuleHandle,std::string&);
ThdAc3Decoder* create_thd_ff61(const char*,ModuleHandle,ModuleHandle,std::string&);
ThdAc3Decoder* create_thd_ff62(const char*,ModuleHandle,ModuleHandle,std::string&);
std::unique_ptr<ThdAc3Decoder> create_thd_ac3_decoder(const char* path,std::string& error);
}

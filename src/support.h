#pragma once
#include "api.h"
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
#include "projectcontext.h"
namespace frzz {
void log(const char* fmt, ...) noexcept;
// A bounded in-memory implementation of the SDK's state stream interface.
class State : public ProjectStateContext {
public:
  std::vector<std::string> lines;
  size_t cursor{};
  int flag{};
  void AddLine(const char* fmt, ...) override {
    // REAPER uses WDL's %p~ for quoted filenames; ordinary printf is incorrect.
    std::vector<char> buf(65536);
    va_list args; va_start(args,fmt);
    ProjectContextFormatString(buf.data(),buf.size(),fmt,args); va_end(args);
    lines.emplace_back(buf.data());
  }
  int GetLine(char* buf,int size) override {
    if (cursor>=lines.size() || size<=0) return -1;
    const auto& s=lines[cursor++];
    if (s.size()>=static_cast<size_t>(size)) { buf[0]=0; return -1; }
    memcpy(buf,s.c_str(),s.size()+1); return 0;
  }
  INT64 GetOutputSize() override { INT64 n=0; for(auto& s:lines) n+=s.size()+1; return n; }
  int GetTempFlag() override {return flag;}
  void SetTempFlag(int v) override {flag=v;}
};
}

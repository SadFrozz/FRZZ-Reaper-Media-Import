#include "subtitle_parser.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <regex>

namespace frzz {
std::string read_utf8(const char* path) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("Cannot open subtitle file");
  f.seekg(0, std::ios::end);
  const auto size=f.tellg();
  if(size<0 || size>16*1024*1024) throw std::runtime_error("Subtitle size invalid or exceeds limit (16 MiB)");
  f.seekg(0);
  std::string data(static_cast<size_t>(size),'\0');
  if(!data.empty() && !f.read(data.data(),size)) throw std::runtime_error("Cannot read subtitle file");
  return data;
}
static std::string trim(std::string s) {
  const auto first=s.find_first_not_of(" \t");
  return first==std::string::npos ? "" : s.substr(first,s.find_last_not_of(" \t")-first+1);
}
static std::string lower(std::string s) {
  for(char& c:s)if(c>='A' && c<='Z')c+=('a'-'A');
  return s;
}
static bool valid_utf8(const std::string& data) {
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
static std::vector<std::string> lines_of(std::string data) {
  if(data.find('\0')!=std::string::npos || !valid_utf8(data))
    throw std::runtime_error("Expected UTF-8 subtitles");
  if(data.compare(0,3,"\xef\xbb\xbf")==0)data.erase(0,3);
  std::istringstream input(data);std::string line;std::vector<std::string> lines;
  while(std::getline(input,line)) {
    if(!line.empty() && line.back()=='\r')line.pop_back();
    lines.push_back(std::move(line));
  }
  return lines;
}
static std::string remove_tags(std::string text) {
  for(size_t at=0;(at=text.find('{',at))!=std::string::npos;) {
    const auto end=text.find('}',at+1);if(end==std::string::npos)break;
    text.erase(at,end-at+1);
  }
  return text;
}
static void hard_breaks(std::string& text) {
  for(size_t at=0;(at=text.find("\\N",at))!=std::string::npos;++at)text.replace(at,2,"\n");
}
static double ass_time(const std::string& value) {
  static const std::regex pattern(R"(^\s*([0-9]+):([0-9]{2}):([0-9]{2})\.([0-9]{2})$)");
  std::smatch m;
  if(!std::regex_match(value,m,pattern))throw std::runtime_error("Invalid ASS/SSA timestamp");
  const int hours=std::stoi(m[1]),minutes=std::stoi(m[2]),seconds=std::stoi(m[3]),fraction=std::stoi(m[4]);
  if(minutes>59 || seconds>59)throw std::runtime_error("Invalid ASS/SSA timestamp range");
  return hours*3600.0+minutes*60.0+seconds+fraction/100.0;
}
static std::vector<std::string> split_format(const std::string& s) {
  std::vector<std::string> result;std::istringstream input(s);std::string field;
  while(std::getline(input,field,','))result.push_back(lower(trim(field)));
  return result;
}
// Split non-text columns from both ends, preserving commas in the Text column.
static std::vector<std::string> split_event(const std::string& s,size_t count,size_t text_index) {
  std::vector<std::string> out(count);size_t left=0,right=s.size();
  for(size_t i=0;i<text_index;++i) {
    auto comma=s.find(',',left);if(comma==std::string::npos)throw std::runtime_error("Incomplete ASS/SSA Dialogue");
    out[i]=s.substr(left,comma-left);left=comma+1;
  }
  for(size_t i=count-1;i>text_index;--i) {
    if(right==0)throw std::runtime_error("Incomplete ASS/SSA Dialogue");
    auto comma=s.rfind(',',right-1);
    if(comma==std::string::npos || comma<left)throw std::runtime_error("Incomplete ASS/SSA Dialogue");
    out[i]=s.substr(comma+1,right-comma-1);right=comma;
  }
  out[text_index]=s.substr(left,right-left);return out;
}
std::vector<Cue> parse_ass(const std::string& contents) {
  const auto lines=lines_of(contents);
  std::vector<std::string> format={"layer","start","end","style","name","marginl","marginr","marginv","effect","text"};
  bool in_events=false;
  std::vector<Cue> result;
  for(const auto& line:lines) {
    const auto structural=lower(trim(line));
    if(!structural.empty() && structural.front()=='[')in_events=structural=="[events]";
    if(in_events && structural.compare(0,7,"format:")==0) {
      format=split_format(trim(line).substr(7));continue;
    }
    if(line.compare(0,9,"Dialogue:")!=0)continue;
    auto index=[&](const char* name)->int {auto it=std::find(format.begin(),format.end(),name);return it==format.end()?-1:static_cast<int>(it-format.begin());};
    const int start=index("start"),end=index("end"),text_index=index("text");
    int actor=index("actor");if(actor<0)actor=index("name");
    if(start<0 || end<0 || text_index<0)throw std::runtime_error("Events Format must contain Start, End and Text");
    auto fields=split_event(line.substr(9),format.size(),static_cast<size_t>(text_index));
    Cue cue{ass_time(fields[start]),ass_time(fields[end]),remove_tags(fields[text_index])};
    if(cue.end<=cue.start)throw std::runtime_error("Subtitle cue end must follow start");
    if(actor>=0 && !fields[actor].empty())cue.text="["+fields[actor]+"] "+cue.text;
    hard_breaks(cue.text);result.push_back(std::move(cue));
  }
  if(result.empty())throw std::runtime_error("No ASS/SSA Dialogue events");
  return result;
}
std::vector<Cue> parse_srt(const std::string& contents) {
  const auto lines=lines_of(contents);std::vector<Cue> result;
  // Keep the v1.3 timestamp spelling and 10-following-line limit.
  static const std::regex pattern(R"(([0-9]{2}):([0-9]{2}):([0-9]{2}),([0-9]{3}) --> ([0-9]{2}):([0-9]{2}):([0-9]{2}),([0-9]{3}))");
  for(size_t i=0;i<lines.size();++i) {
    if(lines[i].find("-->")==std::string::npos)continue;
    std::smatch match;if(!std::regex_search(lines[i],match,pattern))continue;
    auto timestamp=[&](size_t first) {
      const int h=std::stoi(match[first]),m=std::stoi(match[first+1]),s=std::stoi(match[first+2]),ms=std::stoi(match[first+3]);
      if(m>59 || s>59)throw std::runtime_error("Invalid SRT timestamp range");
      return h*3600.0+m*60.0+s+ms/1000.0;
    };
    Cue cue{timestamp(1),timestamp(5),{}};
    if(cue.end<=cue.start)throw std::runtime_error("Subtitle cue end must follow start");
    for(size_t n=1;n<=10 && i+n<lines.size();++n) {
      if(lines[i+n].empty())break;
      if(n>1)cue.text+='\n';
      cue.text+=remove_tags(lines[i+n]);
    }
    // His'key v1.3 applies this in its common item loop, including SRT.
    hard_breaks(cue.text);result.push_back(std::move(cue));
  }
  if(result.empty())throw std::runtime_error("No SRT cues");
  return result;
}
std::vector<Cue> parse_subtitles(const std::string& contents,Format format) {
  if(format==Format::srt)return parse_srt(contents);
  if(format==Format::ass || format==Format::ssa)return parse_ass(contents);
  throw std::runtime_error("Unsupported subtitle extension");
}
}

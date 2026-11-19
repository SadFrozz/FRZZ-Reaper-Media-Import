#include "subtitle_parser.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <charconv>
#include <optional>
#include <string_view>
#include <stdexcept>

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
static bool whitespace(char c) {
  return c==' ' || c=='\t';
}
static std::string_view trim_left(std::string_view text) {
  while(!text.empty() && whitespace(text.front()))text.remove_prefix(1);
  return text;
}
static std::string_view trim(std::string_view text) {
  text=trim_left(text);
  while(!text.empty() && whitespace(text.back()))text.remove_suffix(1);
  return text;
}
static char ascii_lower(char c) {
  return c>='A' && c<='Z' ? static_cast<char>(c+('a'-'A')) : c;
}
static bool starts_with(std::string_view text,std::string_view prefix) {
  return text.substr(0,prefix.size())==prefix;
}
static bool equal_ascii_case(std::string_view text,std::string_view expected) {
  return text.size()==expected.size() &&
    std::equal(text.begin(),text.end(),expected.begin(),[](char a,char b){return ascii_lower(a)==b;});
}
static std::vector<std::string_view> lines_of(const std::string& data) {
  if(data.find('\0')!=std::string::npos || !valid_utf8(data))
    throw std::runtime_error("Expected UTF-8 subtitles");
  std::string_view input=data;
  if(starts_with(input,"\xef\xbb\xbf"))input.remove_prefix(3);
  std::vector<std::string_view> lines;
  while(!input.empty()) {
    const auto end=input.find_first_of("\r\n");
    lines.push_back(input.substr(0,end));
    if(end==std::string_view::npos)break;
    const auto separator=input[end]=='\r' && end+1<input.size() && input[end+1]=='\n' ? 2 : 1;
    input.remove_prefix(end+separator);
  }
  return lines;
}
static bool digit(char c) {return c>='0' && c<='9';}

static bool consume(std::string_view& text,char expected) {
  if(text.empty() || text.front()!=expected)return false;
  text.remove_prefix(1);
  return true;
}
static bool read_number(std::string_view& text,int& number,size_t max_digits) {
  if(text.empty() || !digit(text.front()))return false;
  auto end=text.begin()+1;
  while(end!=text.end() && digit(*end))++end;
  const auto count=static_cast<size_t>(end-text.begin());
  if(count>max_digits || std::from_chars(text.data(),text.data()+count,number).ec!=std::errc{})return false;
  text.remove_prefix(count);
  return true;
}
static std::optional<double> read_time(std::string_view& text,char decimal) {
  int hours,minutes,seconds;
  if(!read_number(text,hours,10) || !consume(text,':') ||
     !read_number(text,minutes,2) || !consume(text,':') ||
     !read_number(text,seconds,2) || minutes>59 || seconds>59)
    return {};
  if(!consume(text,decimal) && !(decimal==',' && consume(text,'.')))return {};
  if(text.empty() || !digit(text.front()))return {};
  int millis=0, place=100;
  while(!text.empty() && digit(text.front())) {
    millis+=(text.front()-'0')*place;
    place/=10;
    text.remove_prefix(1);
  }
  return hours*3600.0+minutes*60.0+seconds+millis/1000.0;
}
static double ass_time(std::string_view field) {
  field=trim(field);
  const auto value=read_time(field,'.');
  if(!value || !field.empty())throw std::runtime_error("Invalid ASS/SSA timestamp");
  return *value;
}

static void append_ass_text(std::string& out,std::string_view text,bool hard_soft_breaks) {
  for(size_t i=0;i<text.size();++i) {
    if(text[i]=='\\' && i+1<text.size()) {
      const char escape=text[i+1];
      if(escape=='N' || escape=='n') {
        out+=escape=='N' || hard_soft_breaks ? '\n' : ' ';
        ++i;
        continue;
      }
      if(escape=='h') {
        out+="\xc2\xa0";
        ++i;
        continue;
      }
    }
    out+=text[i];
  }
}
static void ass_overrides(std::string_view tags,bool& drawing,bool& hard_soft_breaks,bool default_hard_breaks) {
  int depth=0;
  for(size_t i=0;i+1<tags.size();++i) {
    if(tags[i]=='(')++depth;
    if(tags[i]==')' && depth)--depth;
    if(depth || tags[i]!='\\')continue;
    const char name=tags[i+1];
    if(name!='p' && name!='q')continue;
    auto value=tags.substr(i+2);
    // Longer names such as pos and pbo do not change drawing mode.
    if(!value.empty() && ascii_lower(value.front())>='a' && ascii_lower(value.front())<='z')continue;
    value=trim_left(value);
    if(consume(value,'('))value=trim_left(value);
    const bool negative=consume(value,'-');
    if(!negative)consume(value,'+');
    int number=0;
    const bool has_number=read_number(value,number,10);
    if(name=='p')drawing=has_number && !negative && number>0;
    if(name=='q')hard_soft_breaks=has_number ? !negative && number==2 : default_hard_breaks;
  }
}
static std::string ass_plain_text(std::string_view text,bool default_hard_breaks) {
  std::string result;
  bool drawing=false, hard_soft_breaks=default_hard_breaks;
  while(!text.empty()) {
    const auto open=text.find('{');
    const auto close=open==std::string_view::npos ? open : text.find('}',open+1);
    if(close==std::string_view::npos) {
      if(!drawing)append_ass_text(result,text,hard_soft_breaks);
      break;
    }
    if(!drawing)append_ass_text(result,text.substr(0,open),hard_soft_breaks);
    ass_overrides(text.substr(open+1,close-open-1),drawing,hard_soft_breaks,default_hard_breaks);
    text.remove_prefix(close+1);
  }
  return result;
}

struct EventColumns {
  size_t count=10;
  size_t start=1, end=2, text=9;
  size_t actor=4;
};
static EventColumns event_columns(std::string_view format) {
  std::vector<std::string_view> fields;
  for(;;) {
    const auto comma=format.find(',');
    fields.push_back(trim(format.substr(0,comma)));
    if(comma==std::string_view::npos)break;
    format.remove_prefix(comma+1);
  }
  auto index=[&](std::string_view name) {
    return static_cast<size_t>(std::find_if(fields.begin(),fields.end(),
      [&](std::string_view field){return equal_ascii_case(field,name);})-fields.begin());
  };
  EventColumns columns{fields.size(),index("start"),index("end"),index("text"),index("actor")};
  if(columns.actor==columns.count)columns.actor=index("name");
  if(columns.start==columns.count || columns.end==columns.count || columns.text==columns.count)
    throw std::runtime_error("Events Format must contain Start, End and Text");
  return columns;
}
// Split non-text columns from both ends, preserving commas in the Text column.
static std::vector<std::string_view> split_event(std::string_view row,const EventColumns& columns) {
  std::vector<std::string_view> fields(columns.count);
  for(size_t i=0;i<columns.text;++i) {
    const auto comma=row.find(',');
    if(comma==std::string_view::npos)throw std::runtime_error("Incomplete ASS/SSA Dialogue");
    fields[i]=row.substr(0,comma);
    row.remove_prefix(comma+1);
  }
  for(size_t i=columns.count-1;i>columns.text;--i) {
    const auto comma=row.rfind(',');
    if(comma==std::string_view::npos)throw std::runtime_error("Incomplete ASS/SSA Dialogue");
    fields[i]=row.substr(comma+1);
    row=row.substr(0,comma);
  }
  fields[columns.text]=row;
  return fields;
}
std::vector<Cue> parse_ass(const std::string& contents) {
  EventColumns columns;
  // Accept standalone event fragments before any explicit section.
  bool in_events=true, in_script_info=false, hard_soft_breaks=false;
  std::vector<Cue> result;
  for(auto line:lines_of(contents)) {
    const auto structural=trim(line);
    if(!structural.empty() && structural.front()=='[' && structural.back()==']') {
      in_events=equal_ascii_case(structural,"[events]");
      in_script_info=equal_ascii_case(structural,"[script info]");
      continue;
    }
    line=trim_left(line);
    if(in_script_info && equal_ascii_case(line.substr(0,10),"wrapstyle:"))
      hard_soft_breaks=trim(line.substr(10))=="2";
    if(!in_events)continue;
    if(equal_ascii_case(line.substr(0,7),"format:")) {
      columns=event_columns(line.substr(7));
      continue;
    }
    if(!equal_ascii_case(line.substr(0,9),"dialogue:"))continue;
    const auto fields=split_event(line.substr(9),columns);
    Cue cue{ass_time(fields[columns.start]),ass_time(fields[columns.end]),{}};
    // Instantaneous events are valid and retain their exact timing.
    if(cue.end<cue.start)throw std::runtime_error("ASS/SSA cue end cannot precede start");
    if(columns.actor<fields.size() && !fields[columns.actor].empty()) {
      cue.text="["+std::string(fields[columns.actor])+"] ";
      // Preserve the Actor field as metadata, including literal soft escapes.
      for(size_t at=0;(at=cue.text.find("\\N",at))!=std::string::npos;++at)
        cue.text.replace(at,2,"\n");
    }
    cue.text+=ass_plain_text(fields[columns.text],hard_soft_breaks);
    result.push_back(std::move(cue));
  }
  if(result.empty())throw std::runtime_error("No ASS/SSA Dialogue events");
  return result;
}

static bool srt_index(std::string_view line) {
  return !line.empty() && std::all_of(line.begin(),line.end(),digit);
}
static std::optional<Cue> srt_timing(std::string_view line) {
  const auto start=read_time(line,',');
  line=trim_left(line);
  if(!start || !starts_with(line,"-->"))return {};
  line=trim_left(line.substr(3));
  const auto end=read_time(line,',');
  // SubRip also allows position metadata after the end time.
  if(!end || (!line.empty() && !whitespace(line.front())))return {};
  return Cue{*start,*end,{}};
}
static bool srt_style_tag(std::string_view tag) {
  consume(tag,'/');
  const auto name=tag.substr(0,tag.find_first_of(" \t"));
  return equal_ascii_case(name,"b") || equal_ascii_case(name,"i") ||
    equal_ascii_case(name,"u") || equal_ascii_case(name,"s") || equal_ascii_case(name,"font");
}
static void append_srt_text(std::string& out,std::string_view line) {
  // Keep the existing override and hard-break handling for plain REAPER notes.
  const auto last_brace=line.rfind('}');
  for(size_t i=0;i<line.size();++i) {
    if(line[i]=='{' && last_brace!=std::string_view::npos && i<last_brace) {
      i=line.find('}',i+1);
      continue;
    }
    if(line[i]=='<') {
      const auto end=line.find_first_of("<>",i+1);
      if(end!=std::string_view::npos && line[end]=='>' && srt_style_tag(line.substr(i+1,end-i-1))) {
        i=end;
        continue;
      }
    }
    if(line[i]=='\\' && i+1<line.size() && line[i+1]=='N') {
      out+='\n';
      ++i;
    } else out+=line[i];
  }
}
std::vector<Cue> parse_srt(const std::string& contents) {
  const auto lines=lines_of(contents);
  std::vector<Cue> result;
  size_t at=0;
  while(at<lines.size() && trim(lines[at]).empty())++at;
  while(at<lines.size()) {
    if(srt_index(trim(lines[at])))++at;
    auto cue=at<lines.size()?srt_timing(trim(lines[at])):std::nullopt;
    if(!cue)throw std::runtime_error("Expected SRT timing at line "+std::to_string(at+1));
    if(cue->end<cue->start)throw std::runtime_error("SRT cue end cannot precede start");

    const auto body=++at;
    auto last=body;
    bool blank=false;
    for(;at<lines.size();++at) {
      const auto line=trim(lines[at]);
      if(line.empty()) {
        blank=true;
        continue;
      }
      // An empty line separates cues only when the next content is a header.
      if(blank && (srt_index(line) || srt_timing(line)))break;
      last=at+1;
      blank=false;
    }
    // Keep the documented 10-line display limit while reading the whole cue.
    for(auto i=body;i<std::min(last,body+10);++i) {
      if(i!=body)cue->text+='\n';
      if(!trim(lines[i]).empty())append_srt_text(cue->text,lines[i]);
    }
    result.push_back(std::move(*cue));
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

#include "../src/subtitle_parser.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <algorithm>
static void require(bool b,const char* message) {if(!b) throw std::runtime_error(message);}
int main() {
  try {
    const auto cue=frzz::parse_ass("[Events]\nDialogue: 0,0:04:22.67,0:04:26.67,Default,Анна,0,0,0,,{\\i1}Привет, мир!{\\i0}\\NСтрока 2\n");
    require(cue.size()==1,"cue count");
    require(std::abs(cue[0].start-262.67)<1e-10 && std::abs(cue[0].end-266.67)<1e-10,"centisecond timing");
    require(cue[0].text=="[Анна] Привет, мир!\nСтрока 2","actor, Cyrillic, tags, comma and multiline semantics");
    const auto plain=frzz::parse_ass("\xef\xbb\xbf[Events]\r\nComment: ignored\r\nDialogue: 0,0:00:00.00,0:00:01.01,Default,,0,0,0,,Without actor\r\n");
    require(plain.size()==1 && plain[0].text=="Without actor","BOM, CRLF, no actor and comment filtering");
    const auto brackets=frzz::parse_ass("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{a}A{b}B{unfinished");
    require(brackets[0].text=="AB{unfinished","unclosed override semantics");
    const auto spaces=frzz::parse_ass("Dialogue: 0,0:00:00.00,0:00:01.00,Default, Role ,0,0,0,,  text  ");
    require(spaces[0].text=="[ Role ]   text  ","preserve whitespace");
    for(const auto* bad:{"", "Dialogue: 0,broken", "Dialogue: 0,0:00:01.00,0:00:00.00,Default,,0,0,0,,reverse", "Dialogue: 0,0:00:00.001,0:00:01.00,Default,,0,0,0,,wrong precision"}) {
      bool rejected=false;try{frzz::parse_ass(bad);}catch(const std::exception&){rejected=true;}
      require(rejected,"reject malformed or empty input before project mutation");
    }
    const auto srt=frzz::parse_srt("\xef\xbb\xbf" "1\r\n00:01:02,123 --> 00:01:04,456\r\n{tag}Привет\r\nСтрока 2\r\n\r\n2\r\n00:01:05,000 --> 00:01:06,000\r\nEOF без пустой строки");
    require(srt.size()==2 && std::abs(srt[0].start-62.123)<1e-9 && std::abs(srt[0].end-64.456)<1e-9,"SRT millisecond timing, BOM and EOF");
    require(srt[0].text=="Привет\nСтрока 2" && srt[1].text=="EOF без пустой строки","SRT tags, Cyrillic, multiline");
    auto srt_break=frzz::parse_srt("1\n00:00:00,000 --> 00:00:01,000\nA\\NB\n");
    require(srt_break[0].text=="A\nB","shared v1.3 hard-break semantics also apply to SRT");
    std::string long_srt="1\n00:00:00,000 --> 00:00:01,000\n";
    for(int i=1;i<=11;++i)long_srt+="line"+std::to_string(i)+"\n";
    const auto limited=frzz::parse_srt(long_srt);
    require(std::count(limited[0].text.begin(),limited[0].text.end(),'\n')==9 && limited[0].text.find("line10")!=std::string::npos && limited[0].text.find("line11")==std::string::npos,"SRT preserves 10-line cap");
    const auto ssa=frzz::parse_ass("[Events]\nFormat: End, Start, Name, Text\nDialogue: 0:00:03.50,0:00:01.25,Роль,{x}Текст, с запятой\\NСтрока\n");
    require(ssa[0].start==1.25 && ssa[0].end==3.5 && ssa[0].text=="[Роль] Текст, с запятой\nСтрока","SSA reordered Format, Name and text commas");
    const auto middle_text=frzz::parse_ass("[Events]\nFormat: Start, Text, Actor, End\nDialogue: 0:00:01.00,A,B,C,Actor,0:00:02.00");
    require(middle_text[0].text=="[Actor] A,B,C","SSA Text in middle and Actor mapping");
    for(auto& ext:frzz::extensions_list) {
      std::string upper=ext.name;for(char& c:upper)if(c>='a' && c<='z')c-=('a'-'A');
      require(frzz::identify_format("C:\\folder.with.dots\\sample."+upper)==ext.format,"all advertised extensions are matched case-insensitively");
    }
    for(const char* path:{"x.wav","x.flac","x.mp3","x.mp4","x.mkv","x.ass.bak","x","C:\\a.ass\\x"})require(frzz::identify_format(path)==frzz::Format::unsupported,"unrelated formats not claimed");
    bool bad_utf8=false;try{frzz::parse_srt(std::string("\xff"));}catch(const std::exception&){bad_utf8=true;}
    require(bad_utf8,"invalid UTF-8 rejected");
    std::cout << "PASS: ASS/SRT/SSA timing, mapping, Cyrillic, BOM, line endings, multiline, 10-line cap, malformed input and all 25 extension matches\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}

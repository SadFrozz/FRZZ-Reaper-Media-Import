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
    const auto instant=frzz::parse_ass(
      "[Script Info]\nScriptType: v4.00+\n[Events]\n"
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
      "Dialogue: 0,0:00:00.00,0:00:00.00,Default,Роль,0,0,0,,*\n"
      "Dialogue: 0,0:00:11.14,0:00:12.94,Default,,0,0,0,,Timed dialogue");
    require(instant.size()==2,"zero-duration ASS dialogue does not abort the following cues");
    require(instant[0].start==0 && instant[0].end==0 && instant[0].text=="[Роль] *","zero-duration ASS dialogue preserves time and text");
    require(std::abs(instant[1].start-11.14)<1e-10 && std::abs(instant[1].end-12.94)<1e-10,"normal timing after a zero-duration cue");
    const auto brackets=frzz::parse_ass("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{a}A{b}B{unfinished");
    require(brackets[0].text=="AB{unfinished","unclosed override semantics");
    const auto spaces=frzz::parse_ass("Dialogue: 0,0:00:00.00,0:00:01.00,Default, Role ,0,0,0,,  text  ");
    require(spaces[0].text=="[ Role ]   text  ","preserve whitespace");
    for(const auto* bad:{"", "Dialogue: 0,broken", "Dialogue: 0,0:00:01.00,0:00:00.00,Default,,0,0,0,,reverse", "Dialogue: 0,0:00:00.,0:00:01.00,Default,,0,0,0,,missing fraction"}) {
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
    const auto ass_container=frzz::parse_ass(
      "[Script Info]\rDialogue: metadata only\rWrapStyle: 2\r"
      "[Events]\r\tDialogue: 0,0:00:01.001  , 0:00:04.50,Default,,0,0,0,,"
      "До\\nA{\\p1}m 0 0 l 10 10{\\p0}B\\hC{\\q0}\\nD\r"
      "[Aegisub Extradata]\rDialogue: unrelated metadata");
    require(ass_container.size()==1 && std::abs(ass_container[0].start-1.001)<1e-10 &&
      ass_container[0].end==4.5,"ASS section boundaries, indented Dialogue and trimmed millisecond timestamp");
    require(ass_container[0].text=="До\nAB\xc2\xa0" "C D","ASS drawings, hard spaces and script/event wrap mode");
    const auto ass_wrap=frzz::parse_ass(
      "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,A\\nB{\\q2}C\\nD{\\t(\\p1)}E");
    require(ass_wrap[0].text=="A BC\nDE","ASS soft breaks and drawing tags inside transforms");
    const auto ass_actor=frzz::parse_ass(
      "Dialogue: 0,0:00:00.00,0:00:01.00,Default,Studio\\north\\hname\\Nline,0,0,0,,Text");
    require(ass_actor[0].text=="[Studio\\north\\hname\nline] Text","ASS Actor escapes remain literal apart from hard breaks");
    const auto ass_drawing=frzz::parse_ass(
      "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,"
      "A{\\p 1}m 0 0 l 4 4{\\p}B{\\p(1)}m 0 0 l 4 4{\\p 0}C{\\q2}D\\nE{\\q}F\\nG");
    require(ass_drawing[0].text=="ABCD\nEF G","ASS drawing tag parameters and wrap reset");
    const auto ssa_marked=frzz::parse_subtitles(
      "[Script Info]\nScriptType: v4.00\n[Events]\n"
      "Format: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
      "  Dialogue: Marked=1,0:00:00.00 , 0:00:01.25 ,Missing style,Роль,0,0,0,,Привет, SSA",
      frzz::Format::ssa);
    require(ssa_marked.size()==1 && ssa_marked[0].end==1.25 &&
      ssa_marked[0].text=="[Роль] Привет, SSA","classic SSA Marked field and text commas");
    const auto styled=frzz::parse_ass(
      "[V4+ Styles]\nFormat: Name, Fontname\nStyle: Default,Arial\n"
      "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
      "Dialogue: 7,0:00:01.00,0:00:03.00,Missing style,Оля,0,0,0,,Верхняя реплика\n"
      "Dialogue: 0,0:00:01.00,0:00:02.00,Default,Галя,0,0,0,,Нижняя реплика");
    require(styled.size()==2 && styled[0].start==styled[1].start && styled[0].end==3 &&
      styled[1].end==2 && styled[0].text=="[Оля] Верхняя реплика" && styled[1].text=="[Галя] Нижняя реплика",
      "unknown ASS style and coincident starts on different layers retain both plain text events");
    const auto srt_structure=frzz::parse_srt(
      "1\n0:0:0,0-->00:00:00,000 X1:10 X2:100\n"
      "Пример 00:00:07,000 --> 00:00:08,000\n"
      "00:00:09,000 --> 00:00:10,000\n\n"
      "<i>Второй абзац</i> <b>тоже</b> <font color=\"#aaa\">виден</font> <3\n"
      " \t\n2\n00:00:02.5 \t --> 00:00:04,1234\nВторая реплика");
    require(srt_structure.size()==2 && srt_structure[0].start==srt_structure[0].end &&
      srt_structure[1].start==2.5 && std::abs(srt_structure[1].end-4.123)<1e-10,
      "SRT zero length, separator whitespace, flexible timestamp and no phantom cues");
    require(srt_structure[0].text==
      "Пример 00:00:07,000 --> 00:00:08,000\n00:00:09,000 --> 00:00:10,000\n\n"
      "Второй абзац тоже виден <3" && srt_structure[1].text=="Вторая реплика",
      "SRT blank paragraph and markup preserve visible text");
    const auto unnumbered=frzz::parse_srt(
      "0:0:1,000 --> 0:0:2,000\rFirst\r\r0:0:3,000 --> 0:0:4,000\rNext");
    require(unnumbered.size()==2 && unnumbered[0].text=="First" && unnumbered[1].text=="Next",
      "unnumbered SRT cue headers and CR line endings");
    bool bad_srt=false;
    try {frzz::parse_srt("1\n00:00:02,000 --> 00:00:01,000\nReversed");}
    catch(const std::exception&) {bad_srt=true;}
    require(bad_srt,"reversed SRT time still rejected");
    bool bad_srt_index=false;
    try {frzz::parse_srt("1\n00:00:01,000 --> 00:00:02,000\nFirst\n\n2\nMalformed time\nText");}
    catch(const std::exception&) {bad_srt_index=true;}
    require(bad_srt_index,"invalid indexed SRT cue does not silently leave a partial import");

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

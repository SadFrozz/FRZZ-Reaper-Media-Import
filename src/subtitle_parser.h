#pragma once
#include <string>
#include <vector>
#include "formats.h"
namespace frzz {
struct Cue { double start{}, end{}; std::string text; };
std::vector<Cue> parse_ass(const std::string& contents);
std::vector<Cue> parse_srt(const std::string& contents);
std::vector<Cue> parse_subtitles(const std::string& contents, Format format);
std::string read_utf8(const char* path);
}

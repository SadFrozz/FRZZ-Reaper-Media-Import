#include "matroska_eac3.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Bytes=std::vector<unsigned char>;

void append(Bytes& bytes,const Bytes& extra) {
  bytes.insert(bytes.end(),extra.begin(),extra.end());
}

Bytes element(std::initializer_list<unsigned char> id,const Bytes& payload) {
  // These synthetic elements are small enough for a single-byte size.
  Bytes bytes(id);
  bytes.push_back(static_cast<unsigned char>(0x80u+payload.size()));
  append(bytes,payload);
  return bytes;
}

Bytes frame(unsigned blocks) {
  return {0x0b,0x77,0,2,static_cast<unsigned char>(blocks<<4),0x80};
}

Bytes movie(bool irregular,bool group) {
  Bytes entry=element({0xd7},{1});
  append(entry,element({0x83},{2}));
  append(entry,element({0x86},{'A','_','E','A','C','3'}));
  const auto tracks=element({0x16,0x54,0xae,0x6b},element({0xae},entry));
  Bytes fixed{0x81,0,0,4,1};
  append(fixed,frame(3));
  append(fixed,frame(3));
  Bytes tail{0x81,0,0,0};
  append(tail,frame(irregular?0:3));
  Bytes blocks=element({0xa3},fixed);
  if(group)append(blocks,element({0xa0},element({0xa1},tail)));
  else append(blocks,element({0xa3},tail));
  auto segment=tracks;
  append(segment,element({0x1f,0x43,0xb6,0x75},blocks));
  auto file=element({0x1a,0x45,0xdf,0xa3},{});
  append(file,element({0x18,0x53,0x80,0x67},segment));
  return file;
}

struct Fixture {
  std::filesystem::path path=std::filesystem::temp_directory_path()/
    ("frzz-eac3-grid-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".mkv");
  ~Fixture(){std::error_code error; std::filesystem::remove(path,error);}
  bool write(const Bytes& bytes) const {
    std::ofstream file(path,std::ios::binary);
    return static_cast<bool>(file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())));
  }
};
}

int main() {
  Fixture fixture;
  const auto path=fixture.path.u8string();
  if(!fixture.write(movie(false,false)) || !frzz::matroska_eac3_on_timestamp_grid(path.c_str(),1,1000,1)) {
    std::cerr<<"Fixed lacing and whole-millisecond E-AC-3 should allow direct seeking\n";
    return 1;
  }
  if(frzz::matroska_eac3_on_timestamp_grid(path.c_str(),1,1000,2)) {
    std::cerr<<"Unverified E-AC-3 track cannot use the direct clock\n";
    return 1;
  }
  if(!fixture.write(movie(true,false)) || frzz::matroska_eac3_on_timestamp_grid(path.c_str(),1,1000,1)) {
    std::cerr<<"A single short frame invalidates the direct clock\n";
    return 1;
  }
  if(!fixture.write(movie(false,true)) || frzz::matroska_eac3_on_timestamp_grid(path.c_str(),1,1000,1)) {
    std::cerr<<"A BlockGroup can include padding that needs decoding\n";
    return 1;
  }
  auto broken=movie(false,false);
  broken.pop_back();
  if(!fixture.write(broken) || frzz::matroska_eac3_on_timestamp_grid(path.c_str(),1,1000,1)) {
    std::cerr<<"A truncated container cannot prove continuous cadence\n";
    return 1;
  }
}

#include "container_peaks.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
void append(std::vector<unsigned char>& file,int value,int bytes) {
  const auto word=static_cast<std::uint32_t>(value);
  for(int i=0;i<bytes;++i)file.push_back(static_cast<unsigned char>(word>>(8*i)));
}

struct Fixture {
  std::filesystem::path path=std::filesystem::temp_directory_path()/
    ("frzz-peaks-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".reapeaks");
  ~Fixture() {std::error_code error; std::filesystem::remove(path,error);}
  bool write(const std::vector<unsigned char>& bytes) const {
    std::ofstream file(path,std::ios::binary);
    return static_cast<bool>(file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())));
  }
};
}

int main() {
  Fixture fixture;
  const auto path=fixture.path.u8string();
  constexpr int channels=2,rate=48000,frames=640;
  std::vector<unsigned char> bytes{'R','P','K','N',channels,6};
  append(bytes,rate,4);
  append(bytes,0,4);
  append(bytes,0,4);
  for(const auto& band:std::array<std::array<int,2>,6>{{{160,4},{320,2},{-'s',4},{-'s',2},{-'g',96},{-'r',4}}}) {
    append(bytes,band[0],4);
    append(bytes,band[1],4);
  }
  for(int i=0;i<6;++i) {
    append(bytes,4096,2);
    append(bytes,-8192,2);
    append(bytes,8192,2);
    append(bytes,-16384,2);
  }
  for(int i=0;i<6*channels;++i)append(bytes,123456,4);
  bytes.insert(bytes.end(),2*channels*192,42);
  bytes.insert(bytes.end(),2*channels*8,0);
  if(!fixture.write(bytes) || !frzz::complete_peak_file(path.c_str(),channels,rate,frames)) {
    std::cerr<<"Complete waveform with spectrogram was rejected\n";
    return 1;
  }
  if(frzz::complete_peak_file(path.c_str(),channels,rate,frames,path.c_str())) {
    std::cerr<<"Cache with stale media metadata was accepted\n";
    return 1;
  }
  ReaSample maximums[4]{},minimums[4]{};
  std::uint32_t spectral[4]{};
  unsigned char spectrogram[4*192]{};
  PCM_source_peaktransfer_t block{};
  block.peakrate=150;
  block.nchpeaks=channels;
  block.numpeak_points=2;
  block.peaks=maximums;
  block.peaks_minvals=minimums;
  block.extra_requested_data_type='s';
  block.extra_requested_data=spectral;
  block.extra_requested_data_type2='g';
  block.extra_requested_data2=spectrogram;
  frzz::read_peak_file(path.c_str(),channels,rate,frames+1,block);
  if(block.peaks_out!=2 || block.extra_requested_data_out!=2 || block.extra_requested_data_out2!=2 ||
     maximums[0]!=0.125 || maximums[1]!=0.25 || minimums[1]!=-0.5 ||
     spectral[3]!=123456 || spectrogram[sizeof(spectrogram)-1]!=42) {
    std::cerr<<"Waveform channels or extra layers were read incorrectly\n";
    return 1;
  }
  if(frzz::inspect_peak_file(path.c_str(),channels,rate,frames*2,nullptr,true)!=frzz::PeakFileStatus::incomplete ||
     frzz::inspect_peak_file(path.c_str(),channels,rate,frames*2)!=frzz::PeakFileStatus::missing ||
     frzz::inspect_peak_file(path.c_str(),channels,rate,frames+1,nullptr,true)!=frzz::PeakFileStatus::complete) {
    std::cerr<<"Short cache coverage was not classified by source duration\n";
    return 1;
  }
  bytes.pop_back();
  if(!fixture.write(bytes) ||
     frzz::inspect_peak_file(path.c_str(),channels,rate,frames)!=frzz::PeakFileStatus::incomplete) {
    std::cerr<<"Truncated cache was not detected\n";
    return 1;
  }
}

#include "container_peaks.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <vector>

namespace frzz {
namespace {
struct PeakBand {
  std::int64_t division{};
  std::uint32_t count{};
  std::uint64_t offset{};
  unsigned bytes_per_channel{};
};

std::uint32_t uint32(const unsigned char* field) {
  return std::uint32_t(field[0]) | std::uint32_t(field[1])<<8 |
         std::uint32_t(field[2])<<16 | std::uint32_t(field[3])<<24;
}

class PeakFile {
  std::ifstream file;
public:
  std::vector<PeakBand> bands;
  char format{};
  bool incomplete{};
  PeakFile(const char* path,int channels,std::uint32_t rate,std::int64_t frames,const char* media=nullptr,
           bool exact_duration=false)
    :file(std::filesystem::u8path(path),std::ios::binary) {
    // File layout and peak representations: https://www.reaper.fm/sdk/reapeaks.txt
    unsigned char header[18];
    if(!file.read(reinterpret_cast<char*>(header),sizeof(header)) ||
       std::memcmp(header,"RPK",3) || header[4]!=channels || uint32(header+6)!=rate)return;
    if(media) {
#ifdef _WIN32
      struct _stat64 status{};
      const auto filename=std::filesystem::u8path(media);
      if(_wstat64(filename.c_str(),&status)!=0)return;
#else
      struct stat status{};
      if(stat(media,&status)!=0)return;
#endif
      if(uint32(header+10)!=static_cast<std::uint32_t>(status.st_mtime) ||
         uint32(header+14)!=static_cast<std::uint32_t>(status.st_size))return;
    }
    const unsigned peak_bytes=header[3]=='M'?2:(header[3]=='N' || header[3]=='L'?4:0);
    const unsigned mipmaps=header[5];
    if(!peak_bytes || !mipmaps || mipmaps>16)return;
    format=header[3];
    bool short_duration=false;
    std::uint64_t offset=sizeof(header)+mipmaps*8;
    std::vector<PeakBand> parsed;
    for(unsigned i=0;i<mipmaps;++i) {
      unsigned char mipmap[8];
      if(!file.read(reinterpret_cast<char*>(mipmap),sizeof(mipmap)))return;
      const auto code=uint32(mipmap);
      const auto kind=code & 0x80000000u?std::int64_t(code)-0x100000000LL:std::int64_t(code);
      const auto count=uint32(mipmap+4);
      if(i==0) {
        if(kind<=0 || kind>rate)return;
        const auto covered=std::int64_t(count)*kind;
        // A native cache may round a final partial peak to the previous bucket.
        short_duration=covered<frames && (!exact_duration || !count || frames-covered>=kind);
      }
      const auto layer_bytes=std::uint64_t(count)*(kind<0?4:peak_bytes);
      unsigned row_bytes=kind<0?4:peak_bytes;
      if(kind==-static_cast<int>('g'))row_bytes=PCM_source_peaktransfer_t::SPECTROGRAM1_BLOCKSIZE_BYTES;
      if(kind==-static_cast<int>('r'))row_bytes=PCM_source_peaktransfer_t::LOUDNESS_RAW_BYTES;
      // Extra layer counts are stored in 32-bit units, so one row can span several units.
      if(layer_bytes%row_bytes)return;
      parsed.push_back({kind,static_cast<std::uint32_t>(layer_bytes/row_bytes),offset,row_bytes});
      offset+=layer_bytes*channels;
    }
    const auto size=file.seekg(0,std::ios::end).tellg();
    if(size<0)return;
    incomplete=static_cast<std::uint64_t>(size)<offset || (exact_duration && short_duration);
    if(!incomplete && !short_duration)bands=std::move(parsed);
  }

  bool read(const PeakBand& band,std::int64_t first,std::int64_t last,int channels,std::vector<unsigned char>& bytes) {
    const std::uint64_t stride=band.bytes_per_channel*channels;
    const auto size=(last-first+1)*stride;
    if(!size || size>64*1024*1024)return false;
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(static_cast<std::streamoff>(band.offset+first*stride));
    return static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)));
  }
};

double decode_peak(const unsigned char* p,char format) {
  const auto bits=unsigned(p[0]) | unsigned(p[1])<<8;
  const int value=bits & 0x8000?int(bits)-65536:int(bits);
  if(format!='L')return value/32768.0;
  const auto magnitude=std::abs(value);
  const auto sample=magnitude<=24576?magnitude/24576.0:std::exp2((magnitude-24576)/1024.0);
  return std::copysign(sample,value);
}

struct PeakRange {
  double start;
  double step;
  int points;
};

std::int64_t bucket(double frame,std::int64_t division,std::int64_t count) {
  return static_cast<std::int64_t>(std::clamp(std::floor(frame/double(division)),0.0,double(count-1)));
}

std::vector<const PeakBand*> waveform_bands(const PeakFile& file) {
  std::vector<const PeakBand*> waveform;
  for(const auto& band:file.bands)
    if(band.division>0)waveform.push_back(&band);
  return waveform;
}

const PeakBand* best_band(const std::vector<const PeakBand*>& waveform,const PeakRange& range,std::int64_t frames) {
  const auto end=std::min(double(frames),range.start+range.step*range.points);
  auto* best=waveform.front();
  for(auto* band:waveform)
    if(band->count && band->division>=best->division && double(band->division)<=range.step &&
       double(band->count)*double(band->division)>=end)best=band;
  return best;
}

void read_extras(PeakFile& file,const std::vector<const PeakBand*>& waveform,const PeakRange& range,
                 int channels,int out_channels,int type,void* output,int& written) {
  if(!output || (type!='s' && type!='g' && type!='r'))return;
  const int width=PCM_source_peaktransfer_t::extra_blocksize(type);
  std::vector<const PeakBand*> extras;
  for(const auto& band:file.bands)
    if(band.division==-type)extras.push_back(&band);
  if(extras.empty() || extras.size()>waveform.size())return;
  const auto first_normal=waveform.size()-extras.size();
  auto index=first_normal;
  while(index+1<waveform.size() && double(waveform[index+1]->division)<=range.step)++index;
  const auto& band=*extras[index-first_normal];
  if(!band.count || band.bytes_per_channel!=static_cast<unsigned>(width))return;
  const auto division=waveform[index]->division;
  const auto frame_at=[&](int i){return range.start+(i+0.5)*range.step;};
  const auto first=bucket(frame_at(0),division,band.count);
  const auto last=bucket(frame_at(range.points-1),division,band.count);
  std::vector<unsigned char> data;
  if(!file.read(band,first,last,channels,data))return;
  auto* out=static_cast<unsigned char*>(output);
  for(int i=0;i<range.points;++i) {
    const auto row=bucket(frame_at(i),division,band.count)-first;
    for(int ch=0;ch<out_channels;++ch) {
      const auto src_ch=channels==1?0:ch;
      if(src_ch<channels)std::memcpy(out,data.data()+(row*channels+src_ch)*width,width);
      else std::memset(out,0,width);
      out+=width;
    }
  }
  written=range.points;
}
}

PeakFileStatus inspect_peak_file(const char* path,int channels,std::uint32_t rate,std::int64_t frames,const char* media,
                                 bool exact_duration) {
  const PeakFile file(path,channels,rate,frames,media,exact_duration);
  if(file.incomplete)return PeakFileStatus::incomplete;
  return file.bands.empty()?PeakFileStatus::missing:PeakFileStatus::complete;
}

bool complete_peak_file(const char* path,int channels,std::uint32_t rate,std::int64_t frames,const char* media) {
  return inspect_peak_file(path,channels,rate,frames,media)==PeakFileStatus::complete;
}

void read_peak_file(const char* path,int channels,std::uint32_t rate,std::int64_t frames,PCM_source_peaktransfer_t& block) {
  if(!block.peaks || block.nchpeaks<=0 || block.numpeak_points<=0 ||
     !(block.peakrate>0) || !std::isfinite(block.peakrate) || !std::isfinite(block.start_time))return;
  const double start=block.start_time*rate;
  const double step=rate/block.peakrate;
  if(!std::isfinite(start) || !std::isfinite(step) || !(step>0))return;
  const auto count=std::clamp(std::ceil((double(frames)-start)/step),0.0,double(block.numpeak_points));
  const PeakRange range{start,step,static_cast<int>(count)};
  if(!range.points)return;
  // The fallback reader is used by DirectDecoder and shares its duration check.
  PeakFile file(path,channels,rate,frames,nullptr,true);
  if(file.bands.empty())return;
  const auto waveform=waveform_bands(file);
  const auto& band=*best_band(waveform,range,frames);
  const auto first=bucket(start,band.division,band.count);
  const auto last=bucket(start+step*range.points,band.division,band.count);
  const double division=double(band.division);
  std::vector<unsigned char> data;
  if(!file.read(band,first,last,channels,data))return;
  for(int i=0;i<range.points;++i) {
    const auto begin=start+step*i;
    const auto end=begin+step;
    const auto low=bucket(begin,band.division,band.count);
    const auto high=bucket(std::ceil(end/division)*division-1,band.division,band.count);
    for(int ch=0;ch<block.nchpeaks;++ch) {
      double minimum=0,maximum=0;
      const int source_ch=channels==1?0:ch;
      if(end>0 && source_ch<channels) {
        if(begin>=0 && end<=double(frames) && band.bytes_per_channel==4) {
          minimum=std::numeric_limits<double>::infinity();
          maximum=-minimum;
        }
        for(auto row=low;row<=high;++row) {
          const auto* sample=data.data()+((row-first)*channels+source_ch)*band.bytes_per_channel;
          const auto a=decode_peak(sample,file.format);
          const auto b=band.bytes_per_channel==4?decode_peak(sample+2,file.format):a;
          maximum=std::max(maximum,a);
          minimum=std::min(minimum,b);
        }
      }
      const auto output=std::size_t(i)*block.nchpeaks+ch;
      block.peaks[output]=maximum;
      if(block.peaks_minvals)block.peaks_minvals[output]=minimum;
    }
  }
  block.peaks_out=range.points;
  block.output_mode=PCM_source_peaktransfer_t::PEAKTRANSFER_PEAKS_MODE;
  block.peaks_minvals_used=block.peaks_minvals!=nullptr;
  block.extra_requested_data_out=0;
  block.extra_requested_data_out2=0;
  read_extras(file,waveform,range,channels,block.nchpeaks,block.extra_requested_data_type,block.extra_requested_data,block.extra_requested_data_out);
  read_extras(file,waveform,range,channels,block.nchpeaks,block.extra_requested_data_type2,block.extra_requested_data2,block.extra_requested_data_out2);
}
}

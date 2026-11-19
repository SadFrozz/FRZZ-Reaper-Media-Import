#include "thd_ac3.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}
namespace frzz {
namespace {
struct Segment {std::uint64_t offset{},first_unit{};std::uint32_t length{},units{};};
struct SyncPoint {std::uint64_t unit{},offset{};};
struct Index {
#ifdef _WIN32
  HANDLE file=INVALID_HANDLE_VALUE,mapping{};
  const unsigned char* data{};
#else
  int file=-1;
#endif
  std::uint64_t size{},units{};
  int rate{},samples_per_unit{};
  std::string identity;
  std::vector<Segment> segments;
  std::vector<SyncPoint> syncs;
  ~Index(){
#ifdef _WIN32
    if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
#else
    if(file>=0)close(file);
#endif
  }
};
class ReadWindow {
#ifndef _WIN32
  // Both the index scan and each decoder keep their own sequential read buffer.
  // Positioned reads report truncation as a short read instead of faulting on a mapped page.
  std::vector<unsigned char> buffer;
  std::uint64_t start{};
  std::size_t filled{};
#endif
public:
  bool read(const Index& index, std::uint64_t offset, void* destination, std::size_t count) {
    if(offset>index.size || count>index.size-offset)return false;
#ifdef _WIN32
    std::memcpy(destination,index.data+offset,count);
#else
    if(offset<start || offset-start>filled || count>filled-(offset-start)) {
      if(buffer.empty())buffer.resize(64*1024);
      start=offset;
      filled=0;
      const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),index.size-offset));
      while(filled<wanted) {
        const auto read_bytes=pread(index.file,buffer.data()+filled,wanted-filled,static_cast<off_t>(start+filled));
        if(read_bytes<0 && errno==EINTR)continue;
        if(read_bytes<0){filled=0;return false;}
        if(read_bytes==0)break;
        filled+=static_cast<std::size_t>(read_bytes);
      }
      if(count>filled)return false;
    }
    std::memcpy(destination,buffer.data()+(offset-start),count);
#endif
    return true;
  }
};
static unsigned int truehd_unit_size(const unsigned char* header) {
  return ((static_cast<unsigned int>(header[0])<<8|header[1])&0xfff)*2;
}
#ifdef _WIN32
static std::string file_identity(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION info{};if(file==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file,&info))return {};
  std::ostringstream value;value<<info.dwVolumeSerialNumber<<':'<<info.nFileIndexHigh<<':'<<info.nFileIndexLow<<':'<<info.nFileSizeHigh<<':'<<info.nFileSizeLow<<':'<<info.ftLastWriteTime.dwHighDateTime<<':'<<info.ftLastWriteTime.dwLowDateTime;return value.str();
}
static std::string path_identity(const char* path) {
  const auto wide=std::filesystem::u8path(path).wstring();
  HANDLE file=CreateFileW(wide.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return {};const auto value=file_identity(file);CloseHandle(file);return value;
}
#else
static std::string file_identity(int file) {
  struct stat info{};if(file<0 || fstat(file,&info))return {};
  std::ostringstream value;value<<info.st_dev<<':'<<info.st_ino<<':'<<info.st_size<<':'<<info.st_mtimespec.tv_sec<<':'<<info.st_mtimespec.tv_nsec;return value.str();
}
static std::string path_identity(const char* path) {
  struct stat info{};if(stat(path,&info))return {};
  std::ostringstream value;value<<info.st_dev<<':'<<info.st_ino<<':'<<info.st_size<<':'<<info.st_mtimespec.tv_sec<<':'<<info.st_mtimespec.tv_nsec;return value.str();
}
#endif
static int ac3_size(const unsigned char* p) {
  static constexpr int bitrate[]={32,40,48,56,64,80,96,112,128,160,192,224,256,320,384,448,512,576,640};
  const int fscod=p[4]>>6,code=p[4]&63,bsid=p[5]>>3;
  if(fscod>2 || code>37 || bsid>10)return 0;
  const int rate=bitrate[code>>1];
  if(fscod==0)return rate*4;
  if(fscod==1)return ((320*rate/147)+(code&1))*2;
  return rate*6;
}
static std::shared_ptr<Index> build_index(const char* path,std::string& error) {
  auto index=std::make_shared<Index>();
#ifdef _WIN32
  const auto wide=std::filesystem::u8path(path).wstring();
  index->file=CreateFileW(wide.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(index->file==INVALID_HANDLE_VALUE){error="Cannot open THD+AC3 source";return {};}
  index->identity=file_identity(index->file);if(index->identity.empty()){error="Cannot identify THD+AC3 source";return {};}
  LARGE_INTEGER size{};if(!GetFileSizeEx(index->file,&size) || size.QuadPart<16){error="Invalid THD+AC3 source size";return {};}
  index->size=static_cast<std::uint64_t>(size.QuadPart);
  index->mapping=CreateFileMappingW(index->file,nullptr,PAGE_READONLY,0,0,nullptr);
  if(!index->mapping){error="Cannot map THD+AC3 source";return {};}
  index->data=static_cast<const unsigned char*>(MapViewOfFile(index->mapping,FILE_MAP_READ,0,0,0));
  if(!index->data){error="Cannot read THD+AC3 source";return {};}
#else
  index->file=open(path,O_RDONLY);
  if(index->file<0){error="Cannot open THD+AC3 source";return {};}
  index->identity=file_identity(index->file);if(index->identity.empty()){error="Cannot identify THD+AC3 source";return {};}
  struct stat info{};if(fstat(index->file,&info) || info.st_size<16){error="Invalid THD+AC3 source size";return {};}
  index->size=static_cast<std::uint64_t>(info.st_size);
#endif
  ReadWindow reader;
  constexpr std::array<int,3> ac3_rates{{48000,44100,32000}};
  std::array<std::uint64_t,ac3_rates.size()> core_frame_periods{};
  long double core_frame_duration=0;
  std::uint64_t pos=0;Segment current{};bool active=false;
  while(index->size-pos>=8) {
    unsigned char p[8];
    if(!reader.read(*index,pos,p,sizeof(p))){error="Cannot read THD+AC3 frame header";return {};}
    if(p[0]==0x0b && p[1]==0x77) {
      const int bytes=ac3_size(p);
      if(bytes<8 || static_cast<unsigned int>(bytes)>index->size-pos){error="Invalid AC3 frame in combined stream";return {};}
      if(active){index->segments.push_back(current);active=false;}
      const auto fscod=p[4]>>6;
      const auto period=1u<<std::max(0,(p[5]>>3)-8);
      core_frame_periods[fscod]+=period;
      core_frame_duration=std::max(core_frame_duration,1536.0L*period/ac3_rates[fscod]);
      pos+=static_cast<unsigned int>(bytes);continue;
    }
    const unsigned int bytes=truehd_unit_size(p);
    if(bytes<8 || bytes>index->size-pos){error="Invalid TrueHD access unit in combined stream";return {};}
    const bool major=p[4]==0xf8 && p[5]==0x72 && p[6]==0x6f && p[7]==0xba;
    if(major) {
      unsigned char rate_byte;
      if(bytes<=sizeof(p) || !reader.read(*index,pos+sizeof(p),&rate_byte,1)) {
        error="Incomplete TrueHD major sync header";
        return {};
      }
      const int ratebits=rate_byte>>4;
      if(ratebits==0xf){error="Unsupported TrueHD sample rate";return {};}
      const int rate=(ratebits&8?44100:48000)<<(ratebits&7);
      const int samples=40<<(ratebits&7);
      if(index->rate && (index->rate!=rate || index->samples_per_unit!=samples)){error="TrueHD rate changes inside source";return {};}
      index->rate=rate;index->samples_per_unit=samples;
      index->syncs.push_back({index->units,pos});
    }
    if(!active){current={pos,index->units,0,0};active=true;}
    if(bytes>std::numeric_limits<std::uint32_t>::max()-current.length){error="THD+AC3 segment overflow";return {};}
    current.length+=bytes;++current.units;++index->units;pos+=bytes;
  }
  if(active)index->segments.push_back(current);
  if(pos!=index->size || !core_frame_duration || !index->units || index->syncs.empty()) {
    error="Incomplete or unsupported THD+AC3 layout";
    return {};
  }
  if(index->units>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()/index->samples_per_unit)) {
    error="THD+AC3 sample count exceeds supported range";
    return {};
  }
  long double core_duration=0;
  for(std::size_t i=0;i<ac3_rates.size();++i)
    core_duration+=1536.0L*core_frame_periods[i]/ac3_rates[i];
  const long double truehd_duration=static_cast<long double>(index->units)*index->samples_per_unit/index->rate;
  if(std::abs(truehd_duration-core_duration)>2*core_frame_duration) {
    error="AC3 and TrueHD timelines do not match";
    return {};
  }
  return index;
}
static std::mutex index_mutex;
static std::map<std::string,std::weak_ptr<Index>> index_cache;
static std::shared_ptr<Index> get_index(const char* path,std::string& error) {
  std::lock_guard<std::mutex> lock(index_mutex);
  const auto identity=path_identity(path);if(identity.empty()){error="Cannot identify THD+AC3 source";return {};}
  const std::string key=(path?path:"")+std::string("|")+identity;
  if(auto found=index_cache.find(key);found!=index_cache.end())if(auto value=found->second.lock())return value;
  auto value=build_index(path,error);
  if(value && (value->identity!=identity || file_identity(value->file)!=identity || path_identity(path)!=identity)) {
    error="THD+AC3 source changed during indexing";
    return {};
  }
  if(value)index_cache[key]=value;return value;
}
static double sample_value(const AVFrame* frame,int channel,int sample,int channels) {
  const auto format=static_cast<AVSampleFormat>(frame->format);
  const bool planar=format==AV_SAMPLE_FMT_U8P || format==AV_SAMPLE_FMT_S16P || format==AV_SAMPLE_FMT_S32P || format==AV_SAMPLE_FMT_FLTP || format==AV_SAMPLE_FMT_DBLP
    ;
  AVSampleFormat packed=format;
  if(format==AV_SAMPLE_FMT_U8P)packed=AV_SAMPLE_FMT_U8;
  else if(format==AV_SAMPLE_FMT_S16P)packed=AV_SAMPLE_FMT_S16;
  else if(format==AV_SAMPLE_FMT_S32P)packed=AV_SAMPLE_FMT_S32;
  else if(format==AV_SAMPLE_FMT_FLTP)packed=AV_SAMPLE_FMT_FLT;
  else if(format==AV_SAMPLE_FMT_DBLP)packed=AV_SAMPLE_FMT_DBL;
  const unsigned char* base=planar?frame->extended_data[channel]:frame->extended_data[0];
  const int at=planar?sample:sample*channels+channel;
  switch(packed) {
    case AV_SAMPLE_FMT_U8:return (reinterpret_cast<const std::uint8_t*>(base)[at]-128)/128.0;
    case AV_SAMPLE_FMT_S16:return reinterpret_cast<const std::int16_t*>(base)[at]/32768.0;
    case AV_SAMPLE_FMT_S32:return reinterpret_cast<const std::int32_t*>(base)[at]/2147483648.0;
    case AV_SAMPLE_FMT_FLT:return reinterpret_cast<const float*>(base)[at];
    case AV_SAMPLE_FMT_DBL:return reinterpret_cast<const double*>(base)[at];
    default:return 0;
  }
}
class Decoder final:public ThdAc3Decoder {
  ModuleHandle codec_module{},util_module{};
  std::shared_ptr<Index> index;
  const AVCodec* codec{};AVCodecContext* context{};AVPacket* packet{};AVFrame* frame{};
  decltype(&avcodec_free_context) p_free_context{};decltype(&av_packet_free) p_packet_free{};decltype(&av_frame_free) p_frame_free{};
  decltype(&avcodec_send_packet) p_send{};decltype(&avcodec_receive_frame) p_receive{};decltype(&avcodec_flush_buffers) p_flush{};
  decltype(&av_new_packet) p_new_packet{};decltype(&av_packet_unref) p_packet_unref{};decltype(&av_frame_unref) p_frame_unref{};
  std::uint64_t next_unit{},physical{};std::size_t segment_index{};
  int channel_count{};
  ReadWindow reader;
  std::vector<double> samples;
  std::size_t sample_offset{};
  std::int64_t samples_start{};
  bool input_ended{};
  bool locate(const SyncPoint& sync) {
    auto it=std::upper_bound(index->segments.begin(),index->segments.end(),sync.unit,
      [](std::uint64_t unit,const Segment& segment){return unit<segment.first_unit;});
    if(it==index->segments.begin())return false;
    --it;
    if(sync.unit>=it->first_unit+it->units || sync.offset<it->offset || sync.offset>=it->offset+it->length)
      return false;
    segment_index=static_cast<std::size_t>(it-index->segments.begin());
    physical=sync.offset;
    next_unit=sync.unit;
    return true;
  }
  bool append_frame() {
#if LIBAVCODEC_VERSION_MAJOR == 59
    const int count=frame->channels;
#else
    const int count=frame->ch_layout.nb_channels;
#endif
    if(count<=0 || frame->sample_rate!=index->rate || frame->nb_samples<=0)return false;
    const auto format=static_cast<AVSampleFormat>(frame->format);
    if(format!=AV_SAMPLE_FMT_U8 && format!=AV_SAMPLE_FMT_S16 && format!=AV_SAMPLE_FMT_S32 && format!=AV_SAMPLE_FMT_FLT && format!=AV_SAMPLE_FMT_DBL && format!=AV_SAMPLE_FMT_U8P && format!=AV_SAMPLE_FMT_S16P && format!=AV_SAMPLE_FMT_S32P && format!=AV_SAMPLE_FMT_FLTP && format!=AV_SAMPLE_FMT_DBLP)return false;
    if(!channel_count)channel_count=count;
    if(count!=channel_count)return false;
    const auto old=samples.size();samples.resize(old+static_cast<std::size_t>(frame->nb_samples)*count);
    for(int i=0;i<frame->nb_samples;++i)for(int ch=0;ch<count;++ch)samples[old+static_cast<std::size_t>(i)*count+ch]=sample_value(frame,ch,i,count);
    p_frame_unref(frame);return true;
  }
  bool feed_one() {
    if(next_unit>=index->units)return false;
    const auto& current=index->segments[segment_index];
    if(next_unit>=current.first_unit+current.units) {
      ++segment_index;
      if(segment_index>=index->segments.size())return false;
      physical=index->segments[segment_index].offset;
    }
    const auto& segment=index->segments[segment_index];
    const auto end=segment.offset+segment.length;
    unsigned char header[2];
    if(physical>end || end-physical<sizeof(header) || !reader.read(*index,physical,header,sizeof(header)))
      return false;
    const unsigned int bytes=truehd_unit_size(header);
    if(bytes<8 || bytes>end-physical)return false;
    p_packet_unref(packet);
    if(p_new_packet(packet,static_cast<int>(bytes))<0 || !reader.read(*index,physical,packet->data,bytes))
      return false;
    physical+=bytes;
    ++next_unit;
    int sent=p_send(context,packet);
    while(sent==AVERROR(EAGAIN)) {const int got=p_receive(context,frame);if(got<0 || !append_frame())return false;sent=p_send(context,packet);}
    if(sent<0)return false;
    for(;;){const int got=p_receive(context,frame);if(got==AVERROR(EAGAIN) || got==AVERROR_EOF)break;if(got<0 || !append_frame())return false;}
    return true;
  }
  bool seek_to(std::int64_t target) {
    const auto target_unit=static_cast<std::uint64_t>(std::max<std::int64_t>(0,target))/index->samples_per_unit;
    auto it=std::upper_bound(index->syncs.begin(),index->syncs.end(),target_unit,[](std::uint64_t value,const SyncPoint& s){return value<s.unit;});
    if(it==index->syncs.begin())it=index->syncs.begin();else --it;
    if(!locate(*it))return false;
    p_flush(context);
    samples.clear();
    sample_offset=0;
    samples_start=static_cast<std::int64_t>(it->unit)*index->samples_per_unit;
    input_ended=false;
    return true;
  }
public:
  Decoder(const char* path,ModuleHandle codec_mod,ModuleHandle util_mod,std::string& error):codec_module(codec_mod),util_module(util_mod) {
   try {
#define BIND(member,name,module) member=reinterpret_cast<decltype(member)>(module_symbol(module,#name));if(!member){error="Missing FFmpeg symbol " #name;throw 1;}
    decltype(&avcodec_version) version{};BIND(version,avcodec_version,codec_module)
    decltype(&avcodec_find_decoder) find_decoder{};BIND(find_decoder,avcodec_find_decoder,codec_module)
    decltype(&avcodec_alloc_context3) alloc_context{};BIND(alloc_context,avcodec_alloc_context3,codec_module)
    decltype(&avcodec_open2) open_codec{};BIND(open_codec,avcodec_open2,codec_module)
    decltype(&av_packet_alloc) packet_alloc{};BIND(packet_alloc,av_packet_alloc,codec_module)
    decltype(&av_frame_alloc) frame_alloc{};BIND(frame_alloc,av_frame_alloc,util_module)
    BIND(p_free_context,avcodec_free_context,codec_module) BIND(p_packet_free,av_packet_free,codec_module) BIND(p_frame_free,av_frame_free,util_module)
    BIND(p_send,avcodec_send_packet,codec_module) BIND(p_receive,avcodec_receive_frame,codec_module) BIND(p_flush,avcodec_flush_buffers,codec_module)
    BIND(p_new_packet,av_new_packet,codec_module) BIND(p_packet_unref,av_packet_unref,codec_module) BIND(p_frame_unref,av_frame_unref,util_module)
#undef BIND
    if((version()>>16)!=LIBAVCODEC_VERSION_MAJOR){error="FFmpeg codec ABI mismatch";throw 1;}
    index=get_index(path,error);if(!index)throw 1;
    codec=find_decoder(AV_CODEC_ID_TRUEHD);if(!codec){error="FFmpeg TrueHD decoder unavailable";throw 1;}
    context=alloc_context(codec);packet=packet_alloc();frame=frame_alloc();if(!context || !packet || !frame){error="FFmpeg decoder allocation failed";throw 1;}
    if(open_codec(context,codec,nullptr)<0){error="Cannot open FFmpeg TrueHD decoder";throw 1;}
    if(!seek_to(0)){error="Cannot seek to first TrueHD sync";throw 1;}
    while(!channel_count && feed_one()){}
    if(!channel_count){error="TrueHD decoder produced no audio";throw 1;}
    if(!seek_to(0)){error="Cannot reset TrueHD decoder";throw 1;}
   } catch(...) {
    if(frame && p_frame_free)p_frame_free(&frame);
    if(packet && p_packet_free)p_packet_free(&packet);
    if(context && p_free_context)p_free_context(&context);
    throw;
   }
  }
  ~Decoder() override {if(frame)p_frame_free(&frame);if(packet)p_packet_free(&packet);if(context)p_free_context(&context);release_module(util_module);release_module(codec_module);}
  int rate()const override{return index->rate;}int channels()const override{return channel_count;}
  std::int64_t frames()const override{return static_cast<std::int64_t>(index->units)*index->samples_per_unit;}
  int abi()const override{return LIBAVCODEC_VERSION_MAJOR;}
  int read(std::int64_t position,double* output,int length) override {
    if(!output || length<=0 || position<0 || position>=frames())return 0;
    length=static_cast<int>(std::min<std::int64_t>(length,frames()-position));
    if(position!=samples_start && !seek_to(position))return 0;

    // Samples before the first major sync cannot be decoded. Keep their place on the timeline.
    const auto prefix=static_cast<int>(std::clamp<std::int64_t>(samples_start-position,0,length));
    if(prefix) {
      const auto values=static_cast<std::size_t>(prefix)*channels();
      std::fill_n(output,values,0.0);
      if(prefix==length)return prefix;
      output+=values;
      position+=prefix;
      length-=prefix;
    }

    const auto skip=static_cast<std::size_t>(position-samples_start);
    const auto buffered=[&]{return samples.size()/channels()-sample_offset;};
    while(buffered()<skip+static_cast<std::size_t>(length) && !input_ended)
      input_ended=!feed_one();
    if(skip>=buffered())return prefix;

    sample_offset+=skip;
    const int count=static_cast<int>(std::min<std::size_t>(buffered(),length));
    std::memcpy(output,samples.data()+sample_offset*channels(),static_cast<std::size_t>(count)*channels()*sizeof(double));
    sample_offset+=count;
    samples_start=position+count;
    if(sample_offset>4096) {
      samples.erase(samples.begin(),samples.begin()+static_cast<std::ptrdiff_t>(sample_offset*channels()));
      sample_offset=0;
    }
    return prefix+count;
  }
};
}
ThdAc3Decoder* FRZZ_THD_FACTORY(const char* path,ModuleHandle codec,ModuleHandle util,std::string& error) {
  try{return new Decoder(path,codec,util,error);}catch(...){return nullptr;}
}
}

#include "thd_ac3.h"
#include <algorithm>
#include <array>
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
#include <sys/mman.h>
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
#else
  int file=-1;
#endif
  const unsigned char* data{};
  std::uint64_t size{},units{},ac3_frames{};
  int rate{},samples_per_unit{};
  std::string identity;
  std::vector<Segment> segments;
  std::vector<SyncPoint> syncs;
  ~Index(){
#ifdef _WIN32
    if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
#else
    if(data)munmap(const_cast<unsigned char*>(data),static_cast<std::size_t>(size));if(file>=0)close(file);
#endif
  }
};
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
  void* view=mmap(nullptr,static_cast<std::size_t>(index->size),PROT_READ,MAP_PRIVATE,index->file,0);
  if(view==MAP_FAILED){error="Cannot map THD+AC3 source";return {};}
  index->data=static_cast<const unsigned char*>(view);
#endif
  std::uint64_t pos=0;Segment current{};bool active=false;
  while(pos+8<=index->size) {
    const auto p=index->data+pos;
    if(p[0]==0x0b && p[1]==0x77) {
      const int bytes=ac3_size(p);
      if(bytes<8 || pos+bytes>index->size){error="Invalid AC3 frame in combined stream";return {};}
      if(active){index->segments.push_back(current);active=false;}
      pos+=static_cast<unsigned int>(bytes);++index->ac3_frames;continue;
    }
    const unsigned int bytes=((static_cast<unsigned int>(p[0])<<8|p[1])&0xfff)*2;
    if(bytes<8 || pos+bytes>index->size){error="Invalid TrueHD access unit in combined stream";return {};}
    const bool major=p[4]==0xf8 && p[5]==0x72 && p[6]==0x6f && p[7]==0xba;
    if(major) {
      const int ratebits=p[8]>>4;
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
  if(pos!=index->size || !index->ac3_frames || !index->units || !index->rate || index->syncs.empty()) {error="Incomplete or unsupported THD+AC3 layout";return {};}
  const auto truehd_frames=index->units*static_cast<std::uint64_t>(index->samples_per_unit);
  const auto core_frames=index->ac3_frames*1536;
  const auto difference=truehd_frames>core_frames?truehd_frames-core_frames:core_frames-truehd_frames;
  if(difference>3072){error="AC3 and TrueHD timelines do not match";return {};}
  return index;
}
static std::mutex index_mutex;
static std::map<std::string,std::weak_ptr<Index>> index_cache;
static std::shared_ptr<Index> get_index(const char* path,std::string& error) {
  std::lock_guard<std::mutex> lock(index_mutex);
  const auto identity=path_identity(path);if(identity.empty()){error="Cannot identify THD+AC3 source";return {};}
  const std::string key=(path?path:"")+std::string("|")+identity;
  if(auto found=index_cache.find(key);found!=index_cache.end())if(auto value=found->second.lock())return value;
  auto value=build_index(path,error);if(value && value->identity!=identity){error="THD+AC3 source changed during indexing";return {};}
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
  std::int64_t decoded_next{};std::vector<double> samples;std::size_t sample_offset{};std::int64_t samples_start{};bool failed{};
  bool locate(std::uint64_t unit,std::uint64_t exact_offset=UINT64_MAX) {
    auto it=std::upper_bound(index->segments.begin(),index->segments.end(),unit,[](std::uint64_t value,const Segment& s){return value<s.first_unit;});
    if(it==index->segments.begin())return false;--it;segment_index=static_cast<std::size_t>(it-index->segments.begin());
    if(unit>=it->first_unit+it->units)return false;
    physical=it->offset;for(std::uint64_t n=it->first_unit;n<unit;++n)physical+=((index->data[physical]<<8|index->data[physical+1])&0xfff)*2;
    if(exact_offset!=UINT64_MAX && physical!=exact_offset)return false;next_unit=unit;return true;
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
    decoded_next+=frame->nb_samples;p_frame_unref(frame);return true;
  }
  bool feed_one() {
    if(next_unit>=index->units)return false;
    const auto& segment=index->segments[segment_index];
    if(next_unit>=segment.first_unit+segment.units){++segment_index;if(segment_index>=index->segments.size())return false;physical=index->segments[segment_index].offset;}
    const unsigned int bytes=((index->data[physical]<<8|index->data[physical+1])&0xfff)*2;
    p_packet_unref(packet);if(p_new_packet(packet,static_cast<int>(bytes))<0)return false;
    memcpy(packet->data,index->data+physical,bytes);physical+=bytes;++next_unit;
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
    if(!locate(it->unit,it->offset))return false;
    p_flush(context);samples.clear();sample_offset=0;samples_start=decoded_next=static_cast<std::int64_t>(it->unit)*index->samples_per_unit;failed=false;return true;
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
    if(!output || length<=0 || position<0 || position>=frames() || failed)return 0;
    const auto buffered_frames=(samples.size()/channels())-sample_offset;
    if(position<samples_start || position>samples_start+static_cast<std::int64_t>(buffered_frames) || position!=samples_start)if(!seek_to(position)){failed=true;return 0;}
    while(samples_start+static_cast<std::int64_t>((samples.size()/channels())-sample_offset)<position+length && feed_one()){}
    const auto skip=static_cast<std::size_t>(position-samples_start);sample_offset+=skip;samples_start=position;
    const auto available=(samples.size()/channels())-sample_offset;const int count=static_cast<int>(std::min<std::size_t>(available,length));
    memcpy(output,samples.data()+sample_offset*channels(),static_cast<std::size_t>(count)*channels()*sizeof(double));
    sample_offset+=count;samples_start+=count;
    if(sample_offset>4096){samples.erase(samples.begin(),samples.begin()+static_cast<std::ptrdiff_t>(sample_offset*channels()));sample_offset=0;}
    return count;
  }
};
}
ThdAc3Decoder* FRZZ_THD_FACTORY(const char* path,ModuleHandle codec,ModuleHandle util,std::string& error) {
  try{return new Decoder(path,codec,util,error);}catch(...){return nullptr;}
}
}

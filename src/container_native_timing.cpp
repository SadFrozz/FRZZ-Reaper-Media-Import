#include "container_native_timing.h"

#if (defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))) && defined(FRZZ_NATIVE_TIMING)
#include "container_native_video.h"
#include <algorithm>
#include <array>
#include <atomic>
#ifdef _WIN32
#include <intrin.h>
#else
#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#endif
#include <cstring>
#include <memory>
#include <string_view>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace frzz {
namespace {
#ifdef _WIN32
constexpr unsigned factory_call_frame=0xa0;
#else
constexpr unsigned factory_call_frame=0x60;
#endif
struct Choice {
  const void* frame;
  int index;
  int rate;
  int64_t samples;
  const char* alias;
  const std::vector<int>* projection;
};
constexpr int max_projected_channels=128;
thread_local std::array<Choice,8> choices{};
thread_local std::size_t choice_count{};

#define DECLARE(name) decltype(&name) stock_##name{}
DECLARE(avformat_open_input);
DECLARE(avformat_find_stream_info);
DECLARE(avformat_seek_file);
DECLARE(avformat_close_input);
DECLARE(av_read_frame);
DECLARE(av_packet_unref);
DECLARE(av_packet_get_side_data);
DECLARE(av_packet_new_side_data);
DECLARE(avcodec_parameters_to_context);
DECLARE(avcodec_free_context);
DECLARE(avcodec_flush_buffers);
DECLARE(avcodec_send_packet);
DECLARE(avcodec_receive_frame);
DECLARE(av_frame_clone);
DECLARE(av_frame_alloc);
DECLARE(av_frame_get_buffer);
DECLARE(av_frame_move_ref);
DECLARE(av_frame_copy_props);
DECLARE(av_frame_side_data_remove_by_props);
DECLARE(av_channel_layout_index_from_channel);
DECLARE(av_channel_layout_from_mask);
DECLARE(av_channel_layout_copy);
DECLARE(av_channel_layout_custom_init);
DECLARE(av_get_bytes_per_sample);
DECLARE(av_sample_fmt_is_planar);
DECLARE(av_get_packed_sample_fmt);
DECLARE(av_frame_ref);
DECLARE(av_frame_unref);
DECLARE(av_frame_free);
DECLARE(av_frame_make_writable);
DECLARE(av_samples_set_silence);
DECLARE(av_buffer_ref);
DECLARE(av_buffer_unref);
DECLARE(av_rescale_q);
#undef DECLARE

struct Priming {
  int64_t packet_pts{};
  int64_t frame_pts{};
  int64_t packet_pos{};
  int size{};
  std::array<uint8_t,10> data{};
};
std::mutex priming_mutex;
std::unordered_map<std::string_view,Priming> proven_priming;

struct Token {
  AVBufferRef* ref{};
  uint8_t* data{};
  int size{};
  int64_t pts{};
  std::optional<Priming> priming;

  ~Token(){clear();}
  void clear(){stock_av_buffer_unref(&ref);priming.reset();}
  bool retain(const AVPacket& packet) {
    clear();
    if(!packet.buf || !packet.data || packet.size<=0)return false;
    ref=stock_av_buffer_ref(packet.buf);
    data=packet.data;
    size=packet.size;
    pts=packet.pts;
    return ref!=nullptr;
  }
  bool matches(const AVPacket& packet) const {
    return ref && packet.buf && ref->buffer==packet.buf->buffer &&
           packet.data==data && packet.size==size;
  }
};

struct Stream {
  std::mutex mutex;
  Choice choice;
  AVCodecParameters* parameters{};
  AVRational time_base{};
  int64_t start{},origin{},samples{};
  unsigned epoch{};
  bool first_packet=true;
  Token token;
  std::vector<AVChannel> projected_channels;
  uint64_t projected_mask{};
  int projected_count{};
};
struct Decoder {
  std::shared_ptr<Stream> stream;
  AVFrame* held{};
  int64_t silence{};
  unsigned epoch{};
  bool accepted{};
  AVSampleFormat original_format=AV_SAMPLE_FMT_NONE;
  AVSampleFormat projected_format=AV_SAMPLE_FMT_NONE;
  bool restore_format{};
  std::optional<Priming> priming;
  int64_t timestamp_shift{};
  ~Decoder(){stock_av_frame_free(&held);}
  void reset() {
    stock_av_frame_free(&held);
    accepted=false;
    priming.reset();
    epoch=stream->epoch;
  }
};

std::shared_mutex state_mutex;
std::unordered_map<AVFormatContext*,std::shared_ptr<Stream>> inputs;
std::unordered_map<const AVCodecParameters*,std::shared_ptr<Stream>> parameters;
std::unordered_map<AVCodecContext*,std::shared_ptr<Decoder>> decoders;
std::atomic<bool> enabled{};
std::atomic<bool> accepting{};
const void* open_call_return{};
void finish_shutdown() noexcept;

template<class Context,class State>
std::shared_ptr<State> find_state(const std::unordered_map<Context*,std::shared_ptr<State>>& map,Context* context) {
  if(!enabled)return {};
  std::shared_lock<std::shared_mutex> lock(state_mutex);
  const auto it=map.find(context);
  return it!=map.end()?it->second:nullptr;
}

void select(const void* frame,int index,int rate,int64_t samples,const char* alias,const std::vector<int>* projection) noexcept {
  // The existing factory trampoline supplies its original return slot.
  while(choice_count && reinterpret_cast<uintptr_t>(choices[choice_count-1].frame)<=reinterpret_cast<uintptr_t>(frame))
    --choice_count;
  if(accepting && index>=0 && choice_count<choices.size())choices[choice_count++]={frame,index,rate,samples,alias,projection};
}

#ifndef _WIN32
__attribute__((noinline))
#endif
int open_input(AVFormatContext** context,const char* url,const AVInputFormat* fmt,AVDictionary** opts) {
  Choice choice{};
#ifdef _WIN32
  const void* caller=_ReturnAddress();
  const auto return_slot=reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
#else
  const void* caller=__builtin_return_address(0);
  const auto return_slot=reinterpret_cast<uintptr_t>(__builtin_frame_address(0))+sizeof(void*);
#endif
  if(accepting && choice_count && caller==open_call_return) {
    const auto frame=return_slot+factory_call_frame;
    const auto last=choices[choice_count-1];
    if(reinterpret_cast<uintptr_t>(last.frame)==frame) {
      choice=last;
      --choice_count;
    }
  }
  const int ret=stock_avformat_open_input(context,url,fmt,opts);
  if(ret>=0 && choice.frame) {
    try {
      auto stream=std::make_shared<Stream>();
      stream->choice=choice;
      std::lock_guard<std::shared_mutex> lock(state_mutex);
      if(accepting)inputs.emplace(*context,std::move(stream));
    } catch(...) {}
  }
  return ret;
}

int find_stream_info(AVFormatContext* input,AVDictionary** options) {
  const int ret=stock_avformat_find_stream_info(input,options);
  if(ret<0 || !enabled)return ret;
  try {
    std::lock_guard<std::shared_mutex> lock(state_mutex);
    const auto found=inputs.find(input);
    if(found==inputs.end())return ret;
    auto& stream=*found->second;
    if(stream.parameters || stream.choice.index<0 || static_cast<unsigned>(stream.choice.index)>=input->nb_streams)return ret;
    const auto* selected=input->streams[stream.choice.index];
    const auto* par=selected->codecpar;
    if(par->codec_type!=AVMEDIA_TYPE_AUDIO || par->sample_rate!=stream.choice.rate ||
       par->sample_rate<=0 || selected->time_base.num<=0 || selected->time_base.den<=0)return ret;
    stream.time_base=selected->time_base;
    if(stream.choice.samples>0) {
      if(input->start_time==AV_NOPTS_VALUE || selected->start_time==AV_NOPTS_VALUE)return ret;
      const auto origin=stock_av_rescale_q(input->start_time,AVRational{1,AV_TIME_BASE},selected->time_base);
      if(selected->start_time<=origin || origin<0)return ret;
      stream.start=selected->start_time;
      stream.origin=origin;
      stream.samples=stream.choice.samples;
    }
    if(const auto* projection=stream.choice.projection) {
      std::vector<AVChannel> channels;
      uint64_t mask=0;
      bool native_order=true;
      for(const auto speaker:*projection) {
        const auto channel=static_cast<AVChannel>(speaker);
        if(channel<0 || channel==AV_CHAN_UNKNOWN || channel==AV_CHAN_UNUSED ||
           stock_av_channel_layout_index_from_channel(&par->ch_layout,channel)<0 ||
           std::find(channels.begin(),channels.end(),channel)!=channels.end())return ret;
        native_order&=channel<64 && (channels.empty() || channel>channels.back());
        channels.push_back(channel);
        if(channel<64)mask|=uint64_t{1}<<channel;
      }
      stream.projected_count=channels.empty()?par->ch_layout.nb_channels:static_cast<int>(channels.size());
      stream.projected_channels=std::move(channels);
      stream.projected_mask=native_order?mask:0;
    }
    parameters.insert_or_assign(par,found->second);
    stream.parameters=selected->codecpar;
  } catch(...) {}
  return ret;
}

void close_input(AVFormatContext** context) {
  std::shared_ptr<Stream> stream;
  if(context && *context && enabled) {
    std::lock_guard<std::shared_mutex> lock(state_mutex);
    if(const auto found=inputs.find(*context);found!=inputs.end()) {
      stream=std::move(found->second);
      if(stream->parameters)parameters.erase(stream->parameters);
      inputs.erase(found);
    }
  }
  if(stream) {
    std::lock_guard<std::mutex> lock(stream->mutex);
    stream->token.clear();
  }
  stock_avformat_close_input(context);
  finish_shutdown();
}

void restore_codec_format(AVCodecContext* context,const std::shared_ptr<Decoder>& decoder) {
  if(!decoder)return;
  std::lock_guard<std::mutex> lock(decoder->stream->mutex);
  if(decoder->restore_format) {
    context->sample_fmt=decoder->original_format;
    decoder->restore_format=false;
  }
}

int parameters_to_context(AVCodecContext* context,const AVCodecParameters* params) {
  restore_codec_format(context,find_state(decoders,context));
  const int ret=stock_avcodec_parameters_to_context(context,params);
  if(ret>=0 && enabled) {
    try {
      std::lock_guard<std::shared_mutex> lock(state_mutex);
      decoders.erase(context);
      if(const auto found=parameters.find(params);found!=parameters.end()) {
        auto decoder=std::make_shared<Decoder>();
        decoder->stream=found->second;
        decoders.emplace(context,std::move(decoder));
        if(found->second->samples>0)context->pkt_timebase=found->second->time_base;
      }
    } catch(...) {}
  }
  return ret;
}

void free_context(AVCodecContext** context) {
  if(context && enabled) {
    restore_codec_format(*context,find_state(decoders,*context));
    std::lock_guard<std::shared_mutex> lock(state_mutex);
    decoders.erase(*context);
  }
  stock_avcodec_free_context(context);
  finish_shutdown();
}

void flush_buffers(AVCodecContext* context) {
  const auto decoder=find_state(decoders,context);
  restore_codec_format(context,decoder);
  stock_avcodec_flush_buffers(context);
  if(decoder) {
    std::lock_guard<std::mutex> lock(decoder->stream->mutex);
    decoder->reset();
  }
}

int seek_file(AVFormatContext* context,int index,int64_t min,int64_t target,int64_t max,int flags) {
  const int ret=stock_avformat_seek_file(context,index,min,target,max,flags);
  if(ret>=0) {
    if(const auto stream=find_state(inputs,context)) {
      std::lock_guard<std::mutex> lock(stream->mutex);
      stream->token.clear();
      ++stream->epoch;
      stream->first_packet=true;
    }
  }
  return ret;
}

int read_frame(AVFormatContext* context,AVPacket* packet) {
  const int ret=stock_av_read_frame(context,packet);
  if(ret<0)return ret;
  const auto state=find_state(inputs,context);
  if(!state)return ret;
  auto& stream=*state;
  std::lock_guard<std::mutex> lock(stream.mutex);
  if(!stream.parameters || !stream.samples || !stream.first_packet || packet->stream_index!=stream.choice.index)return ret;
  stream.first_packet=false;
  if(packet->pts<=stream.origin || packet->pts>stream.start ||
     (packet->dts!=packet->pts && packet->dts!=AV_NOPTS_VALUE))return ret;
  std::optional<Priming> priming;
  if(packet->pts!=stream.start) {
    if(packet->pos<0)return ret;
    // Matroska codec delay can put the packet before the first audible
    // frame. Keep FFmpeg in charge of dropping the priming samples.
    std::size_t size=0;
    auto* delay=stock_av_packet_get_side_data(packet,AV_PKT_DATA_SKIP_SAMPLES,&size);
    if(!delay) {
      // The Matroska demuxer only supplies this side data on the initial
      // open. A seek back to the same first packet must trim it identically.
      std::lock_guard<std::mutex> lock(priming_mutex);
      const auto prior=proven_priming.find(stream.choice.alias);
      if(prior==proven_priming.end() || prior->second.packet_pts!=packet->pts ||
         prior->second.frame_pts!=stream.start || prior->second.packet_pos!=packet->pos ||
         prior->second.size!=packet->size)return ret;
      size=prior->second.data.size();
      delay=stock_av_packet_new_side_data(packet,AV_PKT_DATA_SKIP_SAMPLES,size);
      if(!delay) {
        stock_av_packet_unref(packet);
        return AVERROR(ENOMEM);
      }
      std::memcpy(delay,prior->second.data.data(),size);
    }
    if(size!=Priming{}.data.size())return ret;
    const AVRational frames{1,stream.choice.rate};
    const auto skip_count=uint32_t{delay[0]} | (uint32_t{delay[1]}<<8) |
                          (uint32_t{delay[2]}<<16) | (uint32_t{delay[3]}<<24);
    const auto skip=stock_av_rescale_q(skip_count,frames,stream.time_base);
    if(skip!=stream.start-packet->pts)return ret;
    priming=Priming{packet->pts,stream.start,packet->pos,packet->size};
    std::memcpy(priming->data.data(),delay,size);
  }
  if(!stream.token.retain(*packet)) {
    const int error=packet->buf?AVERROR(ENOMEM):AVERROR_INVALIDDATA;
    stock_av_packet_unref(packet);
    return error;
  }
  stream.token.priming=priming;
  packet->pts=stream.origin;
  if(packet->dts!=AV_NOPTS_VALUE)packet->dts=stream.origin;
  return ret;
}

int send_packet(AVCodecContext* context,const AVPacket* packet) {
  const auto state=find_state(decoders,context);
  restore_codec_format(context,state);
  const int ret=stock_avcodec_send_packet(context,packet);
  if(ret<0 || !packet || !state)return ret;
  auto& decoder=*state;
  const auto& stream=*decoder.stream;
  std::lock_guard<std::mutex> lock(decoder.stream->mutex);
  if(packet->stream_index==stream.choice.index && packet->pts==stream.origin && stream.token.matches(*packet)) {
    if(decoder.epoch!=stream.epoch)decoder.reset();
    decoder.timestamp_shift=stream.token.pts-stream.origin;
    decoder.accepted=true;
    decoder.priming=stream.token.priming;
    decoder.stream->token.clear();
  }
  return ret;
}

int emit_frame(Decoder& decoder,AVFrame* frame) {
  stock_av_frame_unref(frame);
  if(const int ret=stock_av_frame_ref(frame,decoder.held);ret<0)return ret;
  if(!decoder.silence) {
    stock_av_frame_free(&decoder.held);
    return 0;
  }
  if(const int ret=stock_av_frame_make_writable(frame);ret<0)return ret;
  const auto& stream=*decoder.stream;
  const auto count=static_cast<int>(std::min<int64_t>(decoder.silence,frame->nb_samples));
  if(const int ret=stock_av_samples_set_silence(frame->extended_data,0,count,frame->ch_layout.nb_channels,
                                              static_cast<AVSampleFormat>(frame->format));ret<0)return ret;
  const auto position=stream.samples-decoder.silence;
  const AVRational samples{1,stream.choice.rate};
  const auto start=stock_av_rescale_q(position,samples,stream.time_base);
  const auto end=stock_av_rescale_q(position+count,samples,stream.time_base);
  frame->nb_samples=count;
  frame->pts=frame->pkt_dts=frame->best_effort_timestamp=stream.origin+start;
  frame->duration=end-start;
  decoder.silence-=count;
  NativeVideoTrackCheck::observe_timing(stream.choice.alias);
  return 0;
}

bool enable_channel_projection(const void* raw_context,int source_channels,int main_channels) noexcept {
  try {
    if(!raw_context || main_channels<=0 || main_channels>max_projected_channels)return false;
    auto* context=const_cast<AVCodecContext*>(static_cast<const AVCodecContext*>(raw_context));
    const auto decoder=find_state(decoders,context);
    if(!decoder)return false;
    const auto format=context->sample_fmt;
    if(format!=AV_SAMPLE_FMT_S16 && format!=AV_SAMPLE_FMT_S32 && format!=AV_SAMPLE_FMT_FLT &&
       format!=AV_SAMPLE_FMT_S16P && format!=AV_SAMPLE_FMT_S32P && format!=AV_SAMPLE_FMT_FLTP)return false;
    std::lock_guard<std::mutex> lock(decoder->stream->mutex);
    if(context->ch_layout.nb_channels!=source_channels ||
       decoder->stream->projected_count!=main_channels)return false;
    decoder->original_format=format;
    decoder->projected_format=main_channels>8?stock_av_get_packed_sample_fmt(format):format;
    decoder->restore_format=decoder->projected_format!=format;
    // The host's planar reader only addresses AVFrame::data[0..7]. Tell
    // the native factory to read the projected frame as interleaved audio.
    // Restore the decoder's format before calling its FFmpeg APIs.
    if(decoder->restore_format)context->sample_fmt=decoder->projected_format;
    return true;
  } catch(...) {return false;}
}

int project_frame(const Decoder& decoder,AVFrame* frame) {
  const auto& stream=*decoder.stream;
  if(frame->nb_samples<=0 || !frame->extended_data || frame->format!=decoder.original_format)return AVERROR_INVALIDDATA;
  const int width=stock_av_get_bytes_per_sample(decoder.original_format);
  if(width<=0)return AVERROR_INVALIDDATA;
  const auto channels=static_cast<std::size_t>(stream.projected_count);
  if(channels==0 || channels>max_projected_channels)return AVERROR_INVALIDDATA;
  const bool identity=stream.projected_channels.empty();
  if(identity && frame->ch_layout.nb_channels!=stream.projected_count)return AVERROR_INVALIDDATA;
  if(identity && decoder.original_format==decoder.projected_format) {
    NativeVideoTrackCheck::observe_projection(stream.choice.alias);
    return 0;
  }
  std::array<int,max_projected_channels> indices{};
  for(std::size_t ch=0;ch<channels;++ch) {
    indices[ch]=identity?static_cast<int>(ch):
      stock_av_channel_layout_index_from_channel(&frame->ch_layout,stream.projected_channels[ch]);
    if(indices[ch]<0)return AVERROR_INVALIDDATA;
  }
  const bool planar=stock_av_sample_fmt_is_planar(decoder.original_format)!=0;
  if(planar) {
    for(std::size_t ch=0;ch<channels;++ch)
      if(!frame->extended_data[indices[ch]])return AVERROR_INVALIDDATA;
  } else if(!frame->extended_data[0])return AVERROR_INVALIDDATA;
  auto* output=stock_av_frame_alloc();
  if(!output)return AVERROR(ENOMEM);
  struct Release {
    AVFrame*& frame;
    ~Release(){stock_av_frame_free(&frame);}
  } release{output};
  output->nb_samples=frame->nb_samples;
  output->format=decoder.projected_format;
  output->sample_rate=frame->sample_rate;
  if(identity) {
    if(const int ret=stock_av_channel_layout_copy(&output->ch_layout,&frame->ch_layout);ret<0)return ret;
  } else if(stream.projected_mask) {
    if(const int ret=stock_av_channel_layout_from_mask(&output->ch_layout,stream.projected_mask);ret<0)return ret;
  } else {
    const int ret=stock_av_channel_layout_custom_init(&output->ch_layout,static_cast<int>(channels));
    if(ret<0)return ret;
    for(std::size_t ch=0;ch<channels;++ch)output->ch_layout.u.map[ch].id=stream.projected_channels[ch];
  }
  if(const int ret=stock_av_frame_get_buffer(output,0);ret<0)return ret;
  if(const int ret=stock_av_frame_copy_props(output,frame);ret<0)return ret;
  if(!identity)
    stock_av_frame_side_data_remove_by_props(&output->side_data,&output->nb_side_data,AV_SIDE_DATA_PROP_CHANNEL_DEPENDENT);
  if(stock_av_sample_fmt_is_planar(decoder.projected_format)) {
    const auto length=static_cast<std::size_t>(width)*frame->nb_samples;
    for(std::size_t ch=0;ch<channels;++ch) {
      std::memcpy(output->extended_data[ch],frame->extended_data[indices[ch]],length);
    }
  } else {
    const auto source_stride=static_cast<std::size_t>(width)*frame->ch_layout.nb_channels;
    const auto target_stride=static_cast<std::size_t>(width)*channels;
    for(int sample=0;sample<frame->nb_samples;++sample) {
      auto* target=output->extended_data[0]+static_cast<std::size_t>(sample)*target_stride;
      const auto offset=static_cast<std::size_t>(sample);
      for(std::size_t ch=0;ch<channels;++ch) {
        const auto* source=planar?frame->extended_data[indices[ch]]+offset*width:
          frame->extended_data[0]+offset*source_stride+static_cast<std::size_t>(indices[ch])*width;
        std::memcpy(target+ch*width,source,width);
      }
    }
  }
  stock_av_frame_unref(frame);
  stock_av_frame_move_ref(frame,output);
  NativeVideoTrackCheck::observe_projection(stream.choice.alias);
  return 0;
}

int receive_frame(AVCodecContext* context,AVFrame* frame) {
  const auto state=find_state(decoders,context);
  restore_codec_format(context,state);
  if(state) {
    std::lock_guard<std::mutex> lock(state->stream->mutex);
    if(state->epoch!=state->stream->epoch)state->reset();
    if(state->held)return emit_frame(*state,frame);
  }
  const int ret=stock_avcodec_receive_frame(context,frame);
  if(ret<0 || !enabled || !state)return ret;
  auto& decoder=*state;
  const auto& stream=*decoder.stream;
  std::lock_guard<std::mutex> lock(decoder.stream->mutex);
  if(decoder.projected_format!=AV_SAMPLE_FMT_NONE) {
    if(const int projected=project_frame(decoder,frame);projected<0) {
      stock_av_frame_unref(frame);
      return projected;
    }
  }
  if(decoder.epoch!=stream.epoch || !decoder.accepted ||
     frame->pts!=stream.start-decoder.timestamp_shift || frame->nb_samples<=0)return ret;
  decoder.accepted=false;
  decoder.held=stock_av_frame_clone(frame);
  if(!decoder.held)return AVERROR(ENOMEM);
  decoder.held->pts=stream.start;
  if(decoder.held->pkt_dts!=AV_NOPTS_VALUE)decoder.held->pkt_dts+=decoder.timestamp_shift;
  if(decoder.held->best_effort_timestamp!=AV_NOPTS_VALUE)
    decoder.held->best_effort_timestamp+=decoder.timestamp_shift;
  decoder.silence=stream.samples;
  if(decoder.priming) {
    try {
      std::lock_guard<std::mutex> lock(priming_mutex);
      if(enabled)proven_priming.try_emplace(stream.choice.alias,*decoder.priming);
    } catch(...) {
      return AVERROR(ENOMEM);
    }
    decoder.priming.reset();
  }
  return emit_frame(decoder,frame);
}

#ifdef __APPLE__
void** find_data_slot(const mach_header_64* image,void* value) {
  const auto* command=reinterpret_cast<const load_command*>(image+1);
  uint64_t text_base{};
  for(unsigned i=0;i<image->ncmds;++i) {
    if(command->cmd==LC_SEGMENT_64) {
      const auto* segment=reinterpret_cast<const segment_command_64*>(command);
      if(!strcmp(segment->segname,"__TEXT")) {text_base=segment->vmaddr;break;}
    }
    command=reinterpret_cast<const load_command*>(reinterpret_cast<const unsigned char*>(command)+command->cmdsize);
  }
  void** result{};
  command=reinterpret_cast<const load_command*>(image+1);
  for(unsigned i=0;i<image->ncmds;++i) {
    if(command->cmd==LC_SEGMENT_64) {
      const auto* segment=reinterpret_cast<const segment_command_64*>(command);
      if(!strcmp(segment->segname,"__DATA") && (segment->initprot&VM_PROT_WRITE)) {
        auto** begin=reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(image)+segment->vmaddr-text_base);
        for(std::size_t n=0;n<segment->vmsize/sizeof(void*);++n) {
          if(begin[n]!=value)continue;
          if(result)return nullptr;
          result=begin+n;
        }
      }
    }
    command=reinterpret_cast<const load_command*>(reinterpret_cast<const unsigned char*>(command)+command->cmdsize);
  }
  return result;
}
#endif

bool exchange_slot(void** slot,void* from,void* to) {
#ifdef _WIN32
  return InterlockedCompareExchangePointer(slot,to,from)==from;
#else
  return __atomic_compare_exchange_n(slot,&from,to,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
#endif
}

struct Patch {
  void** slot{};
  void* previous{};
  void* replacement{};
};
std::array<Patch,10> patches;
bool installed{};
bool stop_requested{};
std::mutex install_mutex;

bool install(NativeTimingSlots settings) {
  std::lock_guard<std::mutex> lock(install_mutex);
  if(stop_requested)return false;
  if(installed)return accepting;
  static auto format=retain_loaded_module("avformat",LIBAVFORMAT_VERSION_MAJOR);
  static auto codec=retain_loaded_module("avcodec",LIBAVCODEC_VERSION_MAJOR);
  static auto util=retain_loaded_module("avutil",LIBAVUTIL_VERSION_MAJOR);
  if(!format)format=retain_loaded_module("avformat",LIBAVFORMAT_VERSION_MAJOR);
  if(!codec)codec=retain_loaded_module("avcodec",LIBAVCODEC_VERSION_MAJOR);
  if(!util)util=retain_loaded_module("avutil",LIBAVUTIL_VERSION_MAJOR);
  if(!format || !codec || !util)return false;
  auto* image=reinterpret_cast<unsigned char*>(settings.image);
  const auto* native_malloc=*reinterpret_cast<void**>(image+settings.avutil_malloc);
  if(!native_malloc || native_malloc!=module_symbol(util,"av_malloc"))return false;

#define LOAD(name) stock_##name=reinterpret_cast<decltype(stock_##name)>(module_symbol(util,#name)); if(!stock_##name)return false
  LOAD(av_frame_clone);
  LOAD(av_frame_alloc);
  LOAD(av_frame_get_buffer);
  LOAD(av_frame_move_ref);
  LOAD(av_frame_copy_props);
  LOAD(av_frame_side_data_remove_by_props);
  LOAD(av_channel_layout_index_from_channel);
  LOAD(av_channel_layout_from_mask);
  LOAD(av_channel_layout_copy);
  LOAD(av_channel_layout_custom_init);
  LOAD(av_get_bytes_per_sample);
  LOAD(av_sample_fmt_is_planar);
  LOAD(av_get_packed_sample_fmt);
  LOAD(av_frame_ref);
  LOAD(av_frame_unref);
  LOAD(av_frame_free);
  LOAD(av_frame_make_writable);
  LOAD(av_samples_set_silence);
  LOAD(av_buffer_ref);
  LOAD(av_buffer_unref);
  LOAD(av_rescale_q);
#undef LOAD
  stock_av_packet_unref=reinterpret_cast<decltype(stock_av_packet_unref)>(module_symbol(codec,"av_packet_unref"));
  stock_av_packet_get_side_data=reinterpret_cast<decltype(stock_av_packet_get_side_data)>(
    module_symbol(codec,"av_packet_get_side_data"));
  stock_av_packet_new_side_data=reinterpret_cast<decltype(stock_av_packet_new_side_data)>(
    module_symbol(codec,"av_packet_new_side_data"));
  if(!stock_av_packet_unref || !stock_av_packet_get_side_data || !stock_av_packet_new_side_data)return false;
  std::size_t count=0;
#ifdef _WIN32
#define SLOT(offset,address) reinterpret_cast<void**>(image+settings.format_open+offset)
#else
#define SLOT(offset,address) find_data_slot(reinterpret_cast<const mach_header_64*>(image),address)
#endif
#define PATCH(name,module,offset,replacement) do { \
    auto* address=module_symbol(module,#name); \
    auto** slot=address?SLOT(offset,address):nullptr; \
    if(!slot || *slot!=address)return false; \
    stock_##name=reinterpret_cast<decltype(stock_##name)>(address); \
    patches[count++]={slot,address,reinterpret_cast<void*>(replacement)}; \
  } while(false)
  PATCH(avformat_open_input,format,0x0,open_input);
  PATCH(avformat_find_stream_info,format,0x10,find_stream_info);
  PATCH(avformat_close_input,format,0x18,close_input);
  PATCH(av_read_frame,format,0x20,read_frame);
  PATCH(avformat_seek_file,format,0x90,seek_file);
  PATCH(avcodec_send_packet,codec,0xa8,send_packet);
  PATCH(avcodec_receive_frame,codec,0xb0,receive_frame);
  PATCH(avcodec_flush_buffers,codec,0xc0,flush_buffers);
  PATCH(avcodec_free_context,codec,0xf8,free_context);
  PATCH(avcodec_parameters_to_context,codec,0x118,parameters_to_context);
#undef PATCH
#undef SLOT

  open_call_return=image+settings.open_return;
  std::size_t written=0;
  for(;written<count;++written) {
    const auto& patch=patches[written];
    if(!exchange_slot(patch.slot,patch.previous,patch.replacement))break;
  }
  if(written!=count) {
    for(std::size_t index=0;index<written;++index) {
      const auto& patch=patches[index];
      exchange_slot(patch.slot,patch.replacement,patch.previous);
    }
    return false;
  }
  enabled=true;
  accepting=true;
  installed=true;
  return true;
}

void finish_shutdown() noexcept {
  if(accepting || !enabled)return;
  std::lock_guard<std::mutex> lock(install_mutex);
  if(accepting || !enabled)return;
  {
    std::lock_guard<std::shared_mutex> state_lock(state_mutex);
    if(!inputs.empty() || !decoders.empty())return;
    enabled=false;
  }
  for(const auto& patch:patches)
    exchange_slot(patch.slot,patch.replacement,patch.previous);
  std::lock_guard<std::mutex> priming_lock(priming_mutex);
  proven_priming.clear();
}

void shutdown() noexcept {
  // A live VIDEO may already expect projected channels and interleaved
  // frames. Keep its FFmpeg dispatch until the host closes its contexts.
  {
    std::lock_guard<std::mutex> lock(install_mutex);
    stop_requested=true;
    accepting=false;
  }
  finish_shutdown();
}
}

extern const NativeTimingHook FRZZ_NATIVE_TIMING{install,select,enable_channel_projection,shutdown};
}
#endif

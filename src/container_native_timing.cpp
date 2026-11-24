#include "container_native_timing.h"
#include "video_settings.h"

#if (defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__))) && defined(FRZZ_NATIVE_TIMING)
#include "container_native_video.h"
#include "hevc_preroll.h"
#ifdef FRZZ_MACOS_HDR
#include "video_hdr_macos.h"
#elif defined(FRZZ_WINDOWS_HDR)
#include "video_hdr_windows.h"
#include "video_hdr_symbols.h"
#endif
#ifdef FRZZ_HDR_LEGACY_METADATA
#include "video_hdr_frame.h"
#endif
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
#include <deque>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
}

namespace frzz {
#ifdef FRZZ_MACOS_HDR
using namespace FRZZ_MACOS_HDR;
#elif defined(FRZZ_WINDOWS_HDR)
using namespace FRZZ_WINDOWS_HDR;
#endif
#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
namespace {
// Keep the marker with the output buffers. av_frame_copy_props must not
// turn an unrelated NV12 destination into a buffer we can replace.
class HdrTransferDestination {
public:
  bool bind(ModuleHandle util) noexcept {
    buffer_=reinterpret_cast<decltype(buffer_)>(module_symbol(util,"av_buffer_create"));
    alloc_=reinterpret_cast<decltype(alloc_)>(module_symbol(util,"av_frame_alloc"));
    free_=reinterpret_cast<decltype(free_)>(module_symbol(util,"av_frame_free"));
    unref_=reinterpret_cast<decltype(unref_)>(module_symbol(util,"av_frame_unref"));
    move_=reinterpret_cast<decltype(move_)>(module_symbol(util,"av_frame_move_ref"));
    writable_=reinterpret_cast<decltype(writable_)>(module_symbol(util,"av_frame_is_writable"));
    opaque_=reinterpret_cast<decltype(opaque_)>(module_symbol(util,"av_buffer_get_opaque"));
    release_buffer_=reinterpret_cast<decltype(release_buffer_)>(module_symbol(util,"av_buffer_unref"));
    return buffer_ && alloc_ && free_ && unref_ && move_;
  }

  bool mark(AVFrame& output) const noexcept {
    if(!output.buf[0] || output.buf[1])return false;
#ifdef FRZZ_WINDOWS_HDR
    if(writable_ && opaque_ && release_buffer_)return mark_storage(output);
#endif
    output.buf[1]=buffer_(&marker,1,[](void*,uint8_t*){},nullptr,0);
    return output.buf[1]!=nullptr;
  }

  bool owns(const AVFrame& frame) const noexcept {
    if(frame.format!=AV_PIX_FMT_NV12 || !frame.buf[0] || !frame.buf[1] || frame.buf[1]->data!=&marker)
      return false;
    if(opaque_) {
      if(const auto* storage=opaque_(frame.buf[1]);storage && storage!=opaque_(frame.buf[0]))return false;
    }
    return true;
  }

  bool owns_stock_p010(const AVFrame& frame) const noexcept {
    return frame.format==AV_PIX_FMT_P010LE && stock_destination(frame);
  }

#ifdef FRZZ_WINDOWS_HDR
  bool reusable(AVFrame& frame) const noexcept {
    if(!writable_ || !opaque_ || !owns(frame) || frame.hw_frames_ctx || frame.nb_extended_buf ||
       frame.extended_data!=frame.data || frame.width<=0 || frame.height<=0)return false;
    const auto* storage=opaque_(frame.buf[1]);
    if(!storage || storage!=opaque_(frame.buf[0]) || !writable_(&frame))return false;
    for(int i=2;i<AV_NUM_DATA_POINTERS;++i)
      if(frame.buf[i] || frame.data[i])return false;

    const auto base=reinterpret_cast<uintptr_t>(frame.buf[0]->data);
    const auto size=static_cast<std::size_t>(frame.buf[0]->size);
    const auto width=static_cast<std::size_t>(frame.width);
    std::size_t previous_end=0;
    for(int i=0;i<2;++i) {
      const auto data=reinterpret_cast<uintptr_t>(frame.data[i]);
      if(!data || data<base || frame.linesize[i]<frame.width)return false;
      const auto offset=static_cast<std::size_t>(data-base);
      const auto stride=static_cast<std::size_t>(frame.linesize[i]);
      const auto rows=static_cast<std::size_t>(frame.height>>i);
      if(offset<previous_end || offset>size || width>size-offset || !rows ||
         rows-1>(size-offset-width)/stride)return false;
      previous_end=offset+(rows-1)*stride+width;
    }
    return true;
  }
#endif

  bool can_render(const AVFrame& frame) const noexcept {
    if(frame.hw_frames_ctx)return false;
    return owns(frame) || owns_stock_p010(frame) ||
      ((frame.format==AV_PIX_FMT_NONE || frame.format==AV_PIX_FMT_NV12) && empty(frame));
  }

  int stock_transfer(AVFrame* dst,const AVFrame* src,int flags,
                     int (*transfer)(AVFrame*,const AVFrame*,int)) const noexcept {
    if(!dst || flags || dst->hw_frames_ctx)return transfer(dst,src,flags);
    const auto* layout=src && src->hw_frames_ctx ?
      reinterpret_cast<const AVHWFramesContext*>(src->hw_frames_ctx->data):nullptr;
    const auto* destination=stock_destination(*dst);
    if(!destination && !owns(*dst)) {
      if(!layout || layout->sw_format!=AV_PIX_FMT_P010LE || dst->format!=AV_PIX_FMT_NONE || !empty(*dst))
        return transfer(dst,src,flags);
      const int result=transfer(dst,src,flags);
      if(result>=0 && dst->format==AV_PIX_FMT_P010LE)mark_stock(*dst,*layout);
      return result;
    }
    if(destination && layout && layout->sw_format==dst->format &&
       layout->width==destination->transfer_width && layout->height==destination->transfer_height &&
       dst->width==src->width && dst->height==src->height && writable_ && writable_(dst))
      return transfer(dst,src,flags);
    AVFrame* clean=alloc_();
    if(!clean)return AVERROR(ENOMEM);
    const int result=transfer(clean,src,flags);
    if(result>=0) {
      // This buffer was allocated by the stock transfer into our empty frame.
      // Keep its provenance so a subsequent HDR frame can replace it with NV12.
      if(layout && (clean->format==AV_PIX_FMT_P010LE || clean->format==AV_PIX_FMT_NV12))
        mark_stock(*clean,*layout);
      unref_(dst);
      move_(dst,clean);
    }
    free_(&clean);
    return result;
  }

private:
  inline static uint8_t marker{};
  decltype(&av_buffer_create) buffer_{};
  decltype(&av_frame_alloc) alloc_{};
  decltype(&av_frame_free) free_{};
  decltype(&av_frame_unref) unref_{};
  decltype(&av_frame_move_ref) move_{};
  static bool empty(const AVFrame& frame) noexcept {
    if(frame.nb_extended_buf)return false;
    for(int i=0;i<AV_NUM_DATA_POINTERS;++i)
      if(frame.data[i] || frame.buf[i])return false;
    return true;
  }

  void mark_stock(AVFrame& frame,const AVHWFramesContext& layout) const noexcept {
    if(layout.width>0 && layout.height>0 && frame.buf[0] && !frame.buf[1] && opaque_ && release_buffer_)
      mark_storage(frame,layout.width,layout.height);
  }

  struct PixelStorage {
    decltype(&av_buffer_unref) release;
    AVBufferRef* backing{};
    int transfer_width{},transfer_height{};
    std::atomic<unsigned> references{1};

    static void drop(void* opaque,uint8_t*) noexcept {
      auto* storage=static_cast<PixelStorage*>(opaque);
      if(storage->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete storage;
    }

    static void release_pixels(void* opaque,uint8_t* data) noexcept {
      auto* storage=static_cast<PixelStorage*>(opaque);
      storage->release(&storage->backing);
      drop(opaque,data);
    }
  };

  const PixelStorage* stock_destination(const AVFrame& frame) const noexcept {
    if(!opaque_ || frame.hw_frames_ctx || !frame.buf[0] || !frame.buf[1] ||
       frame.buf[1]->data!=&marker)return nullptr;
    const auto* storage=static_cast<const PixelStorage*>(opaque_(frame.buf[1]));
    return storage && storage==opaque_(frame.buf[0]) && storage->transfer_width>0 ? storage : nullptr;
  }

  bool mark_storage(AVFrame& frame,int transfer_width=0,int transfer_height=0) const noexcept {
    // The pixel buffer and marker share a token. Its address cannot be
    // recycled while either buffer is still referenced by FFmpeg.
    auto* storage=new(std::nothrow) PixelStorage{release_buffer_};
    if(!storage)return false;
    auto* tag=buffer_(&marker,1,PixelStorage::drop,storage,0);
    if(!tag) {
      delete storage;
      return false;
    }
    auto* pixels=buffer_(frame.buf[0]->data,frame.buf[0]->size,PixelStorage::release_pixels,storage,0);
    if(!pixels) {
      release_buffer_(&tag);
      return false;
    }
    storage->backing=frame.buf[0];
    storage->transfer_width=transfer_width;
    storage->transfer_height=transfer_height;
    storage->references.store(2,std::memory_order_relaxed);
    frame.buf[0]=pixels;
    frame.buf[1]=tag;
    return true;
  }

  decltype(&av_frame_is_writable) writable_{};
  decltype(&av_buffer_get_opaque) opaque_{};
  decltype(&av_buffer_unref) release_buffer_{};
};

HdrTransferDestination hdr_destination;
}

#ifdef FRZZ_WINDOWS_HDR
extern "C" bool FRZZ_HDR_API(frzz_hdr_reuse_output)(AVFrame* output) noexcept {
  return hdr_destination.reusable(*output);
}

extern "C" bool FRZZ_HDR_API(frzz_hdr_owns_stock_p010)(const AVFrame* output) noexcept {
  return hdr_destination.owns_stock_p010(*output);
}
#endif

#ifdef FRZZ_MACOS_HDR
bool FRZZ_MACOS_HDR::mark_output(AVFrame* output) noexcept {
#else
extern "C" bool FRZZ_HDR_API(frzz_hdr_mark_output)(AVFrame* output) noexcept {
#endif
  return hdr_destination.mark(*output);
}
#endif
bool FRZZ_CONTAINER_PLANAR_BED(const void*,int,int) noexcept;
namespace {
#ifdef FRZZ_HDR_LEGACY_METADATA
std::mutex hdr_metadata_mutex;
struct LegacyHdrStream {
  AVDOVIDecoderConfigurationRecord config{};
  bool observed_open{};
  bool needs_recovery{};
};
std::unordered_map<const AVCodecParameters*,LegacyHdrStream> hdr_stream_profiles;
std::unordered_map<const AVCodecContext*,AVDOVIDecoderConfigurationRecord> hdr_decoder_profiles;

void remember_hdr_input(const AVFormatContext* input,bool observed_open=false) noexcept {
  if(!input)return;
  try {
    std::lock_guard<std::mutex> lock(hdr_metadata_mutex);
    for(unsigned i=0;i<input->nb_streams;++i) {
      const auto& stream=*input->streams[i];
      if(stream.codecpar->codec_id!=AV_CODEC_ID_HEVC)continue;
      const auto side_data=hdr_stream_side_data(stream);
      const auto config=hdr_dovi_config(side_data.entries,side_data.count);
      if(!observed_open && !config.dv_profile)continue;
      auto& state=hdr_stream_profiles[stream.codecpar];
      state.config=config;
      state.observed_open|=observed_open;
    }
  } catch(...) {}
}

AVDOVIDecoderConfigurationRecord bind_hdr_decoder(const AVCodecContext* codec,const AVCodecParameters* parameters) noexcept {
  try {
    std::lock_guard<std::mutex> lock(hdr_metadata_mutex);
    hdr_decoder_profiles.erase(codec);
    if(const auto config=hdr_stream_profiles.find(parameters);config!=hdr_stream_profiles.end()) {
      auto& state=config->second;
      if(!state.config.dv_profile)state.needs_recovery=true;
      else hdr_decoder_profiles.emplace(codec,state.config);
      return state.config;
    }
  } catch(...) {}
  return {};
}

AVDOVIDecoderConfigurationRecord hdr_decoder_profile(const AVCodecContext* codec) noexcept {
  std::lock_guard<std::mutex> lock(hdr_metadata_mutex);
  const auto config=hdr_decoder_profiles.find(codec);
  return config==hdr_decoder_profiles.end()?AVDOVIDecoderConfigurationRecord{}:config->second;
}

void forget_hdr_decoder(const AVCodecContext* codec) noexcept {
  std::lock_guard<std::mutex> lock(hdr_metadata_mutex);
  hdr_decoder_profiles.erase(codec);
}
#endif

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
#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
DECLARE(av_hwframe_transfer_data);
bool hdr_installed{};
#ifdef FRZZ_WINDOWS_HDR
DECLARE(av_hwdevice_ctx_create);
DECLARE(avcodec_open2);
DECLARE(av_codec_is_decoder);
#endif
#endif
DECLARE(avformat_open_input);
DECLARE(avformat_find_stream_info);
DECLARE(avformat_seek_file);
DECLARE(avformat_close_input);
DECLARE(av_read_frame);
DECLARE(av_packet_unref);
DECLARE(av_packet_get_side_data);
DECLARE(av_packet_new_side_data);
DECLARE(av_packet_clone);
DECLARE(av_packet_free);
DECLARE(avcodec_parameters_to_context);
DECLARE(avcodec_free_context);
#if LIBAVCODEC_VERSION_MAJOR < 62
int (*stock_avcodec_close)(AVCodecContext*){};
#endif
DECLARE(avcodec_flush_buffers);
DECLARE(avcodec_send_packet);
DECLARE(avcodec_receive_frame);
DECLARE(av_frame_clone);
DECLARE(av_frame_alloc);
DECLARE(av_frame_get_buffer);
DECLARE(av_frame_move_ref);
DECLARE(av_frame_copy_props);
#if LIBAVCODEC_VERSION_MAJOR >= 62
DECLARE(av_frame_side_data_remove_by_props);
#else
DECLARE(av_frame_remove_side_data);
#endif
#if LIBAVCODEC_VERSION_MAJOR >= 60
DECLARE(av_channel_layout_index_from_channel);
DECLARE(av_channel_layout_from_mask);
DECLARE(av_channel_layout_copy);
#if LIBAVCODEC_VERSION_MAJOR >= 62
DECLARE(av_channel_layout_custom_init);
#else
DECLARE(av_calloc);
#endif
#endif
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

template<class Audio>
int audio_channels(const Audio& audio) {
#if LIBAVCODEC_VERSION_MAJOR == 59
  return audio.channels;
#else
  return audio.ch_layout.nb_channels;
#endif
}

template<class Audio>
int audio_channel_index(const Audio& audio,AVChannel channel) {
#if LIBAVCODEC_VERSION_MAJOR == 59
  if(channel<0 || channel>=64 || !(audio.channel_layout&(uint64_t{1}<<channel)))return -1;
  int index=0;
  for(int bit=0;bit<channel;++bit)
    if(audio.channel_layout&(uint64_t{1}<<bit))++index;
  return index;
#else
  return stock_av_channel_layout_index_from_channel(&audio.ch_layout,channel);
#endif
}

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
struct HardwareVideoPackets {
  struct Release {
    void operator()(AVPacket* packet) const {stock_av_packet_free(&packet);}
  };
  using Packet=std::unique_ptr<AVPacket,Release>;
  std::mutex mutex;
  struct Submission {
    Packet packet;
    bool discard_output;
    bool nonreference;
    std::uint32_t seek_generation;
  };
  std::deque<Submission> pending;
  bool buffered_nonreference=false;
};
std::unordered_map<AVCodecContext*,std::shared_ptr<HardwareVideoPackets>> video_packets;
struct VideoSeek {
  int stream_index;
  int64_t keep_from;
  unsigned nal_length_size;
  std::uint32_t generation;
  bool saw_idr=false;
  int64_t latest_before_cutoff=AV_NOPTS_VALUE;
};
std::unordered_map<AVFormatContext*,VideoSeek> video_seeks;
struct PrerollPacket {
  AVFormatContext* input;
  HardwareVideoPackets::Packet reference;
  bool nonreference;

  bool matches(const AVPacket& packet) const {
    return packet.buf && reference->buf && reference->buf->buffer==packet.buf->buffer && reference->data==packet.data &&
      reference->pts==packet.pts && reference->size==packet.size && reference->stream_index==packet.stream_index;
  }
};
std::deque<PrerollPacket> preroll_packets;
std::atomic<bool> enabled{};
std::atomic<bool> accepting{};
std::atomic<const void*> open_call_return{};
void finish_shutdown() noexcept;

#ifdef FRZZ_HDR_LEGACY_METADATA
struct LegacyHdrPacket {
  struct Release {
    void operator()(AVBufferRef* buffer) const {stock_av_buffer_unref(&buffer);}
  };
  AVFormatContext* input;
  const AVCodecParameters* parameters;
  std::unique_ptr<AVBufferRef,Release> buffer;
  const uint8_t* data;
  int64_t pts;
  int size,stream_index;
  AVDOVIDecoderConfigurationRecord config;

  bool matches(const AVPacket& packet) const noexcept {
    return buffer->buffer==packet.buf->buffer && data==packet.data && pts==packet.pts &&
      size==packet.size && stream_index==packet.stream_index;
  }
};
std::deque<LegacyHdrPacket> late_hdr_packets;
std::atomic<bool> late_hdr_pending{};

void forget_hdr_input(const AVFormatContext& input) noexcept {
  std::lock_guard<std::mutex> lock(hdr_metadata_mutex);
  for(unsigned i=0;i<input.nb_streams;++i)hdr_stream_profiles.erase(input.streams[i]->codecpar);
  late_hdr_packets.erase(std::remove_if(late_hdr_packets.begin(),late_hdr_packets.end(),
    [&input](const LegacyHdrPacket& marker) {return marker.input==&input;}),late_hdr_packets.end());
  late_hdr_pending=!late_hdr_packets.empty();
}

void remember_late_hdr_packet(AVFormatContext* input,const AVPacket& packet) noexcept {
  if(!packet.buf || !packet.data || packet.size<=0 || packet.stream_index<0 ||
     static_cast<unsigned>(packet.stream_index)>=input->nb_streams)return;
  const auto& stream=*input->streams[packet.stream_index];
  if(stream.codecpar->codec_id!=AV_CODEC_ID_HEVC)return;
  const auto side_data=hdr_stream_side_data(stream);
  const auto config=hdr_dovi_config(side_data.entries,side_data.count);
  if(config.dv_profile!=5 && config.dv_profile!=7 && config.dv_profile!=8)return;
  try {
    std::lock_guard<std::mutex> lock(hdr_metadata_mutex);
    if(!accepting)return;
    const auto known=hdr_stream_profiles.find(stream.codecpar);
    if(known!=hdr_stream_profiles.end() && known->second.observed_open && known->second.config.dv_profile &&
       !known->second.needs_recovery)return;
    std::unique_ptr<AVBufferRef,LegacyHdrPacket::Release> reference(stock_av_buffer_ref(packet.buf));
    if(!reference)return;
    // A decoder opened before the timer installed the hooks can prove the
    // stream through the original packet reference. Retain only a bounded
    // number of candidates until the decoder consumes a matching packet.
    late_hdr_packets.push_back({input,stream.codecpar,std::move(reference),packet.data,packet.pts,packet.size,
                                packet.stream_index,config});
    constexpr std::size_t max_late_hdr_packets=32;
    if(late_hdr_packets.size()>max_late_hdr_packets)late_hdr_packets.pop_front();
    late_hdr_pending=true;
  } catch(...) {}
}

void bind_late_hdr_decoder(const AVCodecContext* codec,const AVPacket& packet) noexcept {
  if(!late_hdr_pending || !packet.buf)return;
  try {
    std::lock_guard<std::mutex> lock(hdr_metadata_mutex);
    if(!accepting || hdr_decoder_profiles.find(codec)!=hdr_decoder_profiles.end())return;
    const auto found=std::find_if(late_hdr_packets.begin(),late_hdr_packets.end(),
      [&packet](const LegacyHdrPacket& marker) {return marker.matches(packet);});
    if(found==late_hdr_packets.end())return;
    hdr_decoder_profiles.try_emplace(codec,found->config);
    // Keep the marker within the rolling window so another decoder that had
    // already opened this stream can prove the same or a subsequent packet.
    auto& stream=hdr_stream_profiles[found->parameters];
    stream.config=found->config;
    stream.needs_recovery=true;
  } catch(...) {}
}
#endif

template<class Context,class State>
std::shared_ptr<State> find_state(const std::unordered_map<Context*,std::shared_ptr<State>>& map,Context* context) {
  if(!enabled)return {};
  std::shared_lock<std::shared_mutex> lock(state_mutex);
  const auto it=map.find(context);
  return it!=map.end()?it->second:nullptr;
}

bool is_hevc_video(const AVCodecContext* context) {
  return context && context->codec_type==AVMEDIA_TYPE_VIDEO && context->codec_id==AV_CODEC_ID_HEVC;
}

bool is_hardware_hevc(const AVCodecContext* context) {
  if(!is_hevc_video(context))return false;
  if(context->hwaccel)return true;
  // A frame-threaded decoder can expose its hardware surfaces to the caller
  // while keeping hwaccel on the worker context.
  if(!(context->active_thread_type&FF_THREAD_FRAME) || !context->codec ||
     std::strcmp(context->codec->name,"hevc")!=0)return false;
  const auto* frames=context->hw_frames_ctx?
    reinterpret_cast<const AVHWFramesContext*>(context->hw_frames_ctx->data):nullptr;
  return frames && frames->device_ctx && frames->device_ctx->type!=AV_HWDEVICE_TYPE_NONE &&
    frames->format!=AV_PIX_FMT_NONE && frames->format==context->pix_fmt;
}

bool has_buffered_video_packets(AVCodecContext* context) {
  if(!is_hevc_video(context))return false;
  const auto packets=find_state(video_packets,context);
  if(!packets)return false;
  std::lock_guard<std::mutex> lock(packets->mutex);
  // A format change can update the hardware state while accepted input
  // still needs to reach the decoder, including a queued drain packet.
  return !packets->pending.empty() || packets->buffered_nonreference;
}

void clear_video_preroll(AVFormatContext* context) {
  const auto first=std::remove_if(preroll_packets.begin(),preroll_packets.end(),
    [context](const PrerollPacket& marker) {return marker.input==context;});
  preroll_packets.erase(first,preroll_packets.end());
}

void record_video_seek(AVFormatContext* context,int index,int64_t min,int64_t target,
                       int64_t max,int flags,int result) {
  std::lock_guard<std::shared_mutex> lock(state_mutex);
  video_seeks.erase(context);
  clear_video_preroll(context);
  const auto settings=video_settings();
  if(!settings.hevc_seek_optimization() || !accepting || result<0 || index!=-1 || flags!=AVSEEK_FLAG_BACKWARD ||
     min!=std::numeric_limits<int64_t>::min() || max!=std::numeric_limits<int64_t>::max())return;

  const AVStream* video=nullptr;
  for(unsigned i=0;i<context->nb_streams;++i) {
    const auto* stream=context->streams[i];
    if(stream->codecpar->codec_type!=AVMEDIA_TYPE_VIDEO || (stream->disposition&AV_DISPOSITION_ATTACHED_PIC))continue;
    if(video)return;
    video=stream;
  }
  if(!video || video->codecpar->codec_id!=AV_CODEC_ID_HEVC || video->time_base.num<=0 ||
     video->time_base.den<=0 || video->avg_frame_rate.num<=0 || video->avg_frame_rate.den<=0)return;
  // A reordered VFR stream can carry a long display interval in a different
  // packet's duration. Let the host choose every frame for those streams.
  if(video->r_frame_rate.num<=0 || video->r_frame_rate.den<=0 ||
     av_cmp_q(video->avg_frame_rate,video->r_frame_rate)!=0)return;

  constexpr AVRational microseconds{1,AV_TIME_BASE};
  const AVRational frame_period{video->avg_frame_rate.den,video->avg_frame_rate.num};
  // REAPER's playback clock can advance before it seeks the video decoder.
  // Keep those earlier frames available for the host's timestamp selection.
#ifdef __APPLE__
  // VideoToolbox can still be finishing the previous decode when the host
  // requests a seek, leaving several hundred milliseconds between targets.
  constexpr int64_t playback_seek_margin_us=750000;
#else
  constexpr int64_t playback_seek_margin_us=160000;
#endif
  const int64_t margin=std::max(playback_seek_margin_us,stock_av_rescale_q(4,frame_period,microseconds));
  if(target<=margin)return;
  const int64_t keep_from=stock_av_rescale_q(target-margin,microseconds,video->time_base);
  unsigned nal_length_size=0;
#if defined(__APPLE__) || LIBAVCODEC_VERSION_MAJOR >= 62
  if(video->codecpar->extradata_size>0)
    nal_length_size=hevc_preroll::single_layer_length_size(video->codecpar->extradata,video->codecpar->extradata_size);
#endif
  if(keep_from>0)try {
    video_seeks.emplace(context,VideoSeek{video->index,keep_from,nal_length_size,settings.seek_generation});
  } catch(...) {}
}

bool seek_nonreference(VideoSeek& seek,const AVPacket& packet,const AVCodecParameters& codec) noexcept {
  if(!seek.nal_length_size)return false;
  if(packet.dts!=AV_NOPTS_VALUE && packet.dts>=seek.keep_from) {
    seek.nal_length_size=0;
    return false;
  }
  for(int i=0;i<packet.side_data_elems;++i) {
    if(packet.side_data[i].type==AV_PKT_DATA_NEW_EXTRADATA) {
      seek.nal_length_size=0;
      return false;
    }
  }
  using hevc_preroll::Picture;
  const bool check_parameters=!seek.saw_idr && codec.extradata_size>0;
  const auto unit=packet.size>0 && !(packet.flags&AV_PKT_FLAG_CORRUPT) ?
    hevc_preroll::classify(packet.data,packet.size,seek.nal_length_size,
                          check_parameters?codec.extradata:nullptr,check_parameters?codec.extradata_size:0) :
    hevc_preroll::AccessUnit{};
  if(!seek.saw_idr) {
    seek.saw_idr=unit.picture==Picture::idr && unit.current_parameters && (packet.flags&AV_PKT_FLAG_KEY);
    if(!seek.saw_idr)seek.nal_length_size=0;
    return false;
  }
  if(unit.picture==Picture::unsuitable || unit.parameters || unit.irap)seek.nal_length_size=0;
  return seek.nal_length_size && unit.picture==Picture::nonreference && !packet.side_data_elems &&
    !(packet.flags&(AV_PKT_FLAG_KEY|AV_PKT_FLAG_DISCARD));
}

void read_video_preroll(AVFormatContext* context,const AVPacket& packet) {
  if(!accepting)return;
  std::lock_guard<std::shared_mutex> lock(state_mutex);
  const auto found=video_seeks.find(context);
  const auto settings=video_settings();
  if(!accepting || !settings.hevc_seek_optimization() || found==video_seeks.end() ||
     found->second.generation!=settings.seek_generation || found->second.stream_index!=packet.stream_index)return;
  auto& seek=found->second;
  const bool nonreference=seek_nonreference(seek,packet,*context->streams[packet.stream_index]->codecpar);
  // A long frame can still cover the seek position even if its PTS is earlier.
  if(!packet.buf || !packet.data || packet.size<=0 || packet.pts==AV_NOPTS_VALUE ||
     (packet.flags&(AV_PKT_FLAG_DISCARD|AV_PKT_FLAG_CORRUPT)))return;
  if(packet.pts<=seek.keep_from)seek.latest_before_cutoff=std::max(seek.latest_before_cutoff,packet.pts);
  if(!hevc_preroll::ends_before_or_at(packet.pts,packet.duration,seek.keep_from))return;
  HardwareVideoPackets::Packet reference(stock_av_packet_clone(&packet));
  if(!reference)return;
  try {
    // A demuxer may read audio packets or more video packets before the
    // corresponding hardware decoder consumes this reference.
    preroll_packets.push_back({context,std::move(reference),nonreference});
    constexpr std::size_t maximum_preroll_packets=128;
    if(preroll_packets.size()>maximum_preroll_packets)preroll_packets.pop_front();
  } catch(...) {}
}

std::optional<bool> consume_video_preroll(const AVPacket& packet,const VideoSettings& settings) {
  std::lock_guard<std::shared_mutex> lock(state_mutex);
  const auto found=std::find_if(preroll_packets.begin(),preroll_packets.end(),
    [&packet](const PrerollPacket& marker) {return marker.matches(packet);});
  if(found==preroll_packets.end())return std::nullopt;
  const auto seek=video_seeks.find(found->input);
  // Matroska can report the nominal duration even when the next PTS is far
  // away. Keep a candidate for the host to display across that gap.
  const bool earlier=settings.hevc_seek_optimization() && seek!=video_seeks.end() &&
    seek->second.generation==settings.seek_generation && packet.pts<seek->second.latest_before_cutoff;
  const bool nonreference=found->nonreference && !packet.side_data_elems &&
    !(packet.flags&(AV_PKT_FLAG_KEY|AV_PKT_FLAG_CORRUPT|AV_PKT_FLAG_DISCARD));
  for(int i=0;i<packet.side_data_elems;++i) {
    if(packet.side_data[i].type!=AV_PKT_DATA_NEW_EXTRADATA)continue;
    // A packet can acquire new decoder parameters after the demux callback.
    // Markers already queued for this seek must keep decoding their pictures.
    const auto* input=found->input;
    if(seek!=video_seeks.end())seek->second.nal_length_size=0;
    for(auto& marker:preroll_packets) {
      if(marker.input==input)marker.nonreference=false;
    }
    break;
  }
  preroll_packets.erase(found);
  return earlier?std::optional{nonreference}:std::nullopt;
}

class ScopedNonreferenceDiscard {
public:
  ScopedNonreferenceDiscard(AVCodecContext& codec,bool nonreference) noexcept
    : codec_(codec),saved_(codec.skip_frame),changed_(nonreference && saved_<AVDISCARD_NONREF) {
    if(changed_)codec_.skip_frame=AVDISCARD_NONREF;
  }
  ~ScopedNonreferenceDiscard() {
    if(changed_)codec_.skip_frame=saved_;
  }

private:
  AVCodecContext& codec_;
  AVDiscard saved_;
  bool changed_;
};

int submit_video_packet(AVCodecContext* context,HardwareVideoPackets* packets,
                        const AVPacket* packet,bool discard_output,bool nonreference) {
  AVPacket decode_only;
  if(discard_output) {
    decode_only=*packet;
    decode_only.flags|=AV_PKT_FLAG_DISCARD;
    packet=&decode_only;
  }
  const ScopedNonreferenceDiscard discard(*context,nonreference);
  const int ret=stock_avcodec_send_packet(context,packet);
  if(packets && ret!=AVERROR(EAGAIN))packets->buffered_nonreference=ret==0 && nonreference;
  return ret;
}

int submit_pending(AVCodecContext* context,HardwareVideoPackets& packets) {
  const auto& submission=packets.pending.front();
  const bool use_preroll=submission.discard_output &&
    submission.seek_generation==video_settings().seek_generation;
  const int ret=submit_video_packet(context,&packets,submission.packet.get(),
                                    use_preroll,use_preroll && submission.nonreference);
  if(ret!=AVERROR(EAGAIN))packets.pending.pop_front();
  return ret;
}

int send_hardware_video_packet(AVCodecContext* context,const AVPacket* packet,
                               bool discard_output=false,bool nonreference=false,
                               std::uint32_t seek_generation=0) {
  constexpr std::size_t max_pending_packets=32;
  auto packets=find_state(video_packets,context);
  if(!packets && nonreference) {
    try {
      auto state=std::make_shared<HardwareVideoPackets>();
      std::lock_guard<std::shared_mutex> lock(state_mutex);
      if(accepting)packets=video_packets.try_emplace(context,std::move(state)).first->second;
    } catch(...) {}
    if(!packets)nonreference=false;
  }
  if(!packets) {
    const int ret=submit_video_packet(context,nullptr,packet,discard_output,nonreference);
    if(ret!=AVERROR(EAGAIN) || !accepting)return ret;
    try {
      auto pending=std::make_shared<HardwareVideoPackets>();
      std::lock_guard<std::shared_mutex> lock(state_mutex);
      if(!accepting)return ret;
      packets=video_packets.try_emplace(context,std::move(pending)).first->second;
    } catch(...) {return AVERROR(ENOMEM);}
  } else {
    std::lock_guard<std::mutex> lock(packets->mutex);
    if(!packets->pending.empty() && !packets->pending.back().packet)return AVERROR_EOF;
    while(!packets->pending.empty()) {
      const int pending_result=submit_pending(context,*packets);
      if(pending_result==AVERROR(EAGAIN))break;
      if(pending_result==AVERROR_INVALIDDATA)continue;
      if(pending_result<0)return pending_result;
    }
    if(packets->pending.empty()) {
      const int ret=submit_video_packet(context,packets.get(),packet,discard_output,nonreference);
      if(ret!=AVERROR(EAGAIN))return ret;
    }
  }

  // REAPER can move on to the next input packet after EAGAIN. Retain the
  // original until this decoder can accept it, preserving decode order.
  std::lock_guard<std::mutex> lock(packets->mutex);
  if(!packets->pending.empty() && !packets->pending.back().packet)return AVERROR_EOF;
  if(packets->pending.size()>=max_pending_packets)return AVERROR(EAGAIN);
  const bool has_packet=packet && (packet->data || packet->size || packet->side_data_elems);
  HardwareVideoPackets::Packet copy(has_packet?stock_av_packet_clone(packet):nullptr);
  if(has_packet && !copy)return AVERROR(ENOMEM);
  // Cloning a packet without a payload can add a padding buffer. Preserve
  // null data so FFmpeg still accepts drain and side-data-only packets.
  if(copy && !packet->data && packet->size==0)copy->data=nullptr;
  try {packets->pending.push_back({std::move(copy),discard_output,nonreference,seek_generation});}
  catch(...) {return AVERROR(ENOMEM);}
  return 0;
}

int receive_hardware_video_frame(AVCodecContext* context,AVFrame* frame) {
  const auto packets=find_state(video_packets,context);
  if(!packets)return stock_avcodec_receive_frame(context,frame);
  std::lock_guard<std::mutex> lock(packets->mutex);
  const auto receive=[&]() {
    // A successful send may buffer the packet until receive. EAGAIN means
    // FFmpeg needs new input; no accepted packet still needs this setting.
    const ScopedNonreferenceDiscard discard(*context,packets->buffered_nonreference);
    const int ret=stock_avcodec_receive_frame(context,frame);
    if(ret<0)packets->buffered_nonreference=false;
    return ret;
  };
  int ret=receive();
  while(!packets->pending.empty() && ret==AVERROR(EAGAIN)) {
    const int pending_result=submit_pending(context,*packets);
    if(pending_result==AVERROR_INVALIDDATA)continue;
    if(pending_result<0)return pending_result;
    ret=receive();
  }
  return ret;
}

void select(const void* frame,int index,int rate,int64_t samples,const char* alias,const std::vector<int>* projection) noexcept {
  // The existing factory trampoline supplies its original return slot.
  while(choice_count && reinterpret_cast<uintptr_t>(choices[choice_count-1].frame)<=reinterpret_cast<uintptr_t>(frame))
    --choice_count;
  if(accepting && open_call_return && index>=0 && choice_count<choices.size())
    choices[choice_count++]={frame,index,rate,samples,alias,projection};
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
#ifdef FRZZ_HDR_LEGACY_METADATA
  if(ret>=0 && accepting && hdr_installed && context)remember_hdr_input(*context,true);
#endif
#ifdef FRZZ_WINDOWS_HDR
  if(ret>=0 && accepting && hdr_installed && context && video_settings().hdr_to_sdr)prepare_windows_hdr_stream(*context);
#endif
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
#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
  if(accepting && hdr_installed) {
#ifdef FRZZ_HDR_LEGACY_METADATA
    remember_hdr_input(input);
#endif
#ifdef FRZZ_MACOS_HDR
    if(video_settings().hdr_to_sdr)prepare_macos_hdr(input);
#else
    if(video_settings().hdr_to_sdr)prepare_windows_hdr_stream(input);
#endif
  }
#endif
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
           audio_channel_index(*par,channel)<0 ||
           std::find(channels.begin(),channels.end(),channel)!=channels.end())return ret;
        native_order&=channel<64 && (channels.empty() || channel>channels.back());
        channels.push_back(channel);
        if(channel<64)mask|=uint64_t{1}<<channel;
      }
      stream.projected_count=channels.empty()?audio_channels(*par):static_cast<int>(channels.size());
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
#ifdef FRZZ_HDR_LEGACY_METADATA
  if(context && *context && enabled)forget_hdr_input(**context);
#endif
  if(context && *context && enabled) {
    std::lock_guard<std::shared_mutex> lock(state_mutex);
    video_seeks.erase(*context);
    clear_video_preroll(*context);
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
#if defined(FRZZ_HDR_LEGACY_METADATA) || defined(FRZZ_WINDOWS_HDR)
  if(ret>=0 && accepting && hdr_installed) {
#ifdef FRZZ_HDR_LEGACY_METADATA
    [[maybe_unused]] const auto fallback=bind_hdr_decoder(context,params);
#ifdef FRZZ_WINDOWS_HDR
    remember_windows_hdr_parameters(params,&fallback);
#endif
#else
    remember_windows_hdr_parameters(params,nullptr);
#endif
  }
#endif
  if(ret>=0 && enabled) {
    try {
      std::lock_guard<std::shared_mutex> lock(state_mutex);
      decoders.erase(context);
      video_packets.erase(context);
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

void forget_codec_context(AVCodecContext* context) {
  if(!context || !enabled)return;
#ifdef FRZZ_HDR_LEGACY_METADATA
  forget_hdr_decoder(context);
#endif
  restore_codec_format(context,find_state(decoders,context));
  std::lock_guard<std::shared_mutex> lock(state_mutex);
  decoders.erase(context);
  video_packets.erase(context);
}

void free_context(AVCodecContext** context) {
  if(context)forget_codec_context(*context);
  stock_avcodec_free_context(context);
  finish_shutdown();
}

#if LIBAVCODEC_VERSION_MAJOR < 62
int close_codec(AVCodecContext* context) {
  forget_codec_context(context);
  const auto ret=stock_avcodec_close(context);
  finish_shutdown();
  return ret;
}
#endif

void flush_buffers(AVCodecContext* context) {
  if(const auto packets=find_state(video_packets,context)) {
    std::lock_guard<std::mutex> lock(packets->mutex);
    packets->pending.clear();
    packets->buffered_nonreference=false;
    stock_avcodec_flush_buffers(context);
    return;
  }
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
  if(video_settings().hevc_seek_optimization())record_video_seek(context,index,min,target,max,flags,ret);
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
  if(ret>=0 && video_settings().hevc_seek_optimization())read_video_preroll(context,*packet);
  if(ret<0)return ret;
#ifdef FRZZ_HDR_LEGACY_METADATA
  if(accepting && hdr_installed)remember_late_hdr_packet(context,*packet);
#endif
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
#ifdef FRZZ_HDR_LEGACY_METADATA
  if(enabled && accepting && hdr_installed && packet && is_hardware_hevc(context))
    bind_late_hdr_decoder(context,*packet);
#endif
  const auto settings=video_settings();
  if(settings.hevc_patches && enabled && is_hardware_hevc(context)) {
    const auto preroll=accepting && packet && settings.skip_hevc_seek_frames?
      consume_video_preroll(*packet,settings):std::nullopt;
    if(preroll) {
      const bool can_discard_nonreference=
#ifdef _WIN32
        LIBAVCODEC_VERSION_MAJOR >= 62 && context->pix_fmt==AV_PIX_FMT_D3D11;
#else
        context->pix_fmt==AV_PIX_FMT_VIDEOTOOLBOX;
#endif
      const bool nonreference=*preroll && can_discard_nonreference &&
        !(context->active_thread_type&FF_THREAD_FRAME) && !(context->flags2&AV_CODEC_FLAG2_CHUNKS);
      return send_hardware_video_packet(context,packet,true,nonreference,settings.seek_generation);
    }
    return send_hardware_video_packet(context,packet);
  }
  if(has_buffered_video_packets(context))return send_hardware_video_packet(context,packet);
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
  if(const int ret=stock_av_samples_set_silence(frame->extended_data,0,count,audio_channels(*frame),
                                              static_cast<AVSampleFormat>(frame->format));ret<0)return ret;
  const auto position=stream.samples-decoder.silence;
  const AVRational samples{1,stream.choice.rate};
  const auto start=stock_av_rescale_q(position,samples,stream.time_base);
  const auto end=stock_av_rescale_q(position+count,samples,stream.time_base);
  frame->nb_samples=count;
  frame->pts=frame->pkt_dts=frame->best_effort_timestamp=stream.origin+start;
#if LIBAVCODEC_VERSION_MAJOR >= 60
  frame->duration=end-start;
#else
  frame->pkt_duration=end-start;
#endif
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
    if(audio_channels(*context)!=source_channels ||
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

int assign_projected_layout(AVFrame& output,const AVFrame& input,const Stream& stream) {
  const bool identity=stream.projected_channels.empty();
#if LIBAVCODEC_VERSION_MAJOR == 59
  output.channels=stream.projected_count;
  output.channel_layout=identity?input.channel_layout:stream.projected_mask;
  return !identity && !output.channel_layout?AVERROR(ENOSYS):0;
#else
  if(identity)return stock_av_channel_layout_copy(&output.ch_layout,&input.ch_layout);
  if(stream.projected_mask)return stock_av_channel_layout_from_mask(&output.ch_layout,stream.projected_mask);
#if LIBAVCODEC_VERSION_MAJOR >= 62
  const int result=stock_av_channel_layout_custom_init(&output.ch_layout,stream.projected_count);
  if(result<0)return result;
#else
  auto* map=static_cast<AVChannelCustom*>(stock_av_calloc(stream.projected_count,sizeof(AVChannelCustom)));
  if(!map)return AVERROR(ENOMEM);
  output.ch_layout.order=AV_CHANNEL_ORDER_CUSTOM;
  output.ch_layout.nb_channels=stream.projected_count;
  output.ch_layout.u.map=map;
#endif
  for(int channel=0;channel<stream.projected_count;++channel)
    output.ch_layout.u.map[channel].id=stream.projected_channels[channel];
  return 0;
#endif
}

int project_frame(const Decoder& decoder,AVFrame* frame) {
  const auto& stream=*decoder.stream;
  if(frame->nb_samples<=0 || !frame->extended_data || frame->format!=decoder.original_format)return AVERROR_INVALIDDATA;
  const int width=stock_av_get_bytes_per_sample(decoder.original_format);
  if(width<=0)return AVERROR_INVALIDDATA;
  const auto channels=static_cast<std::size_t>(stream.projected_count);
  if(channels==0 || channels>max_projected_channels)return AVERROR_INVALIDDATA;
  const bool identity=stream.projected_channels.empty();
  if(identity && audio_channels(*frame)!=stream.projected_count)return AVERROR_INVALIDDATA;
  if(identity && decoder.original_format==decoder.projected_format) {
    NativeVideoTrackCheck::observe_projection(stream.choice.alias);
    return 0;
  }
  std::array<int,max_projected_channels> indices{};
  for(std::size_t ch=0;ch<channels;++ch) {
    indices[ch]=identity?static_cast<int>(ch):audio_channel_index(*frame,stream.projected_channels[ch]);
    if(indices[ch]<0 || indices[ch]>=audio_channels(*frame))return AVERROR_INVALIDDATA;
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
  if(const int ret=assign_projected_layout(*output,*frame,stream);ret<0)return ret;
  if(const int ret=stock_av_frame_get_buffer(output,0);ret<0)return ret;
  if(const int ret=stock_av_frame_copy_props(output,frame);ret<0)return ret;
#if LIBAVCODEC_VERSION_MAJOR >= 62
  if(!identity)
    stock_av_frame_side_data_remove_by_props(&output->side_data,&output->nb_side_data,AV_SIDE_DATA_PROP_CHANNEL_DEPENDENT);
#else
  if(!identity) {
    stock_av_frame_remove_side_data(output,AV_FRAME_DATA_DOWNMIX_INFO);
    stock_av_frame_remove_side_data(output,AV_FRAME_DATA_MATRIXENCODING);
  }
#endif
  if(stock_av_sample_fmt_is_planar(decoder.projected_format)) {
    const auto length=static_cast<std::size_t>(width)*frame->nb_samples;
    for(std::size_t ch=0;ch<channels;++ch) {
      std::memcpy(output->extended_data[ch],frame->extended_data[indices[ch]],length);
    }
  } else {
    const auto source_stride=static_cast<std::size_t>(width)*audio_channels(*frame);
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

#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
void remember_hdr_frame(AVCodecContext* context,AVFrame* frame) {
  if(!accepting || !hdr_installed || !context || context->codec_id!=AV_CODEC_ID_HEVC)return;
#ifdef FRZZ_HDR_LEGACY_METADATA
  const auto profile=hdr_decoder_profile(context);
  const auto* fallback=&profile;
#else
  const AVDOVIDecoderConfigurationRecord* fallback=nullptr;
#endif
#ifdef FRZZ_MACOS_HDR
  remember_macos_hdr_frame(context,frame,fallback);
#else
  remember_windows_hdr_frame(context,frame,fallback);
#endif
}
#endif

int receive_frame(AVCodecContext* context,AVFrame* frame) {
  if(enabled && ((video_settings().hevc_patches && is_hardware_hevc(context)) || has_buffered_video_packets(context))) {
    const int ret=receive_hardware_video_frame(context,frame);
#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
    if(ret>=0)remember_hdr_frame(context,frame);
#endif
    return ret;
  }
  const auto state=find_state(decoders,context);
  restore_codec_format(context,state);
  if(state) {
    std::lock_guard<std::mutex> lock(state->stream->mutex);
    if(state->epoch!=state->stream->epoch)state->reset();
    if(state->held)return emit_frame(*state,frame);
  }
  const int ret=stock_avcodec_receive_frame(context,frame);
#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
  // With frame threading the returned surface can be hardware even when
  // the caller's codec context does not expose the worker's hwaccel.
  if(ret>=0 && enabled && !state)remember_hdr_frame(context,frame);
#endif
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

#ifdef _WIN32
bool writable_dispatch_region(const MEMORY_BASIC_INFORMATION& page) {
  const DWORD access=page.Protect&0xff;
  return page.State==MEM_COMMIT && !(page.Protect&PAGE_GUARD) &&
    (access==PAGE_READWRITE || access==PAGE_WRITECOPY);
}

bool writable_dispatch_slot(void** slot) {
  MEMORY_BASIC_INFORMATION page{};
  return VirtualQuery(slot,&page,sizeof(page)) && writable_dispatch_region(page);
}

void** find_video_dispatch_slot(unsigned char* image,void* function) {
  const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0)return nullptr;
  const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+dos->e_lfanew);
  if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return nullptr;
  const auto& header=nt->OptionalHeader;
  IMAGE_DATA_DIRECTORY iat{};
  if(header.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_IAT)iat=header.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
  const auto iat_end=static_cast<std::uint64_t>(iat.VirtualAddress)+iat.Size;
  auto* section=IMAGE_FIRST_SECTION(nt);
  void** result{};
  for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
    const auto& current=section[i];
    if(!(current.Characteristics&IMAGE_SCN_MEM_WRITE) ||
       current.Characteristics&(IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE))continue;
    const auto offset=current.VirtualAddress;
    const auto length=current.Misc.VirtualSize;
    if(offset>header.SizeOfImage || length>header.SizeOfImage-offset)continue;
    const auto limit=reinterpret_cast<uintptr_t>(image+offset+length);
    auto cursor=reinterpret_cast<uintptr_t>(image+offset);
    while(cursor<limit) {
      MEMORY_BASIC_INFORMATION page{};
      if(!VirtualQuery(reinterpret_cast<void*>(cursor),&page,sizeof(page)))break;
      const auto next=std::min(limit,reinterpret_cast<uintptr_t>(page.BaseAddress)+page.RegionSize);
      if(next<=cursor)break;
      if(page.AllocationBase==image && writable_dispatch_region(page)) {
        const auto aligned=(cursor+alignof(void*)-1)&~(alignof(void*)-1);
        for(auto address=aligned;address<next && next-address>=sizeof(void*);address+=sizeof(void*)) {
          const auto rva=address-reinterpret_cast<uintptr_t>(image);
          if(iat.Size && rva<iat_end && rva+sizeof(void*)>iat.VirtualAddress)continue;
          auto** slot=reinterpret_cast<void**>(address);
          if(*slot!=function)continue;
          if(result)return nullptr;
          result=slot;
        }
      }
      cursor=next;
    }
  }
  return result;
}
#endif

bool exchange_slot(void** slot,void* from,void* to) {
#ifdef _WIN32
  return writable_dispatch_slot(slot) && InterlockedCompareExchangePointer(slot,to,from)==from;
#else
  return __atomic_compare_exchange_n(slot,&from,to,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
#endif
}

#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
int transfer_hardware_frame(AVFrame* dst,const AVFrame* src,int flags) {
  if(video_settings().hdr_to_sdr && accepting && dst && !flags && hdr_destination.can_render(*dst)) {
#ifdef FRZZ_MACOS_HDR
    if(render_macos_hdr(dst,src,flags))return 0;
#else
    if(render_windows_hdr(dst,src,flags))return 0;
#endif
  }
  return hdr_destination.stock_transfer(dst,src,flags,stock_av_hwframe_transfer_data);
}
#endif

#ifdef FRZZ_WINDOWS_HDR
int open_hardware_codec(AVCodecContext* codec,const AVCodec* implementation,AVDictionary** options) {
  const int result=stock_avcodec_open2(codec,implementation,options);
  if(result>=0 && accepting && hdr_installed && video_settings().hdr_to_sdr && codec && codec->codec_type==AVMEDIA_TYPE_VIDEO &&
     codec->codec_id==AV_CODEC_ID_HEVC && codec->codec && stock_av_codec_is_decoder(codec->codec)) {
#ifdef FRZZ_HDR_LEGACY_METADATA
    const auto fallback=hdr_decoder_profile(codec);
    prepare_windows_hdr_decoder(*codec,&fallback);
#else
    prepare_windows_hdr_decoder(*codec);
#endif
  }
  return result;
}

int create_hardware_device(AVBufferRef** device,AVHWDeviceType type,const char* selector,AVDictionary* options,int flags) {
  const bool prepare=accepting && video_settings().hdr_to_sdr && type==AV_HWDEVICE_TYPE_DXVA2;
  if(prepare)prepare_windows_hdr_device(selector);
  const int result=stock_av_hwdevice_ctx_create(device,type,selector,options,flags);
  if(result>=0 && prepare && accepting && device)calibrate_windows_hdr_device(*device);
  return result;
}
#endif

struct Patch {
  void** slot{};
  void* previous{};
  void* replacement{};
};
std::array<Patch,14> patches;
std::size_t installed_patches{};
ModuleHandle installed_image{};
using LoadedModule=std::unique_ptr<std::remove_pointer_t<ModuleHandle>,decltype(&release_module)>;
bool audio_ready{};
bool stop_requested{};
std::mutex install_mutex;

bool bind_audio(ModuleHandle util,ModuleHandle codec) {
#define LOAD(name) stock_##name=reinterpret_cast<decltype(stock_##name)>(module_symbol(util,#name)); if(!stock_##name)return false
  LOAD(av_frame_clone);
  LOAD(av_frame_alloc);
  LOAD(av_frame_get_buffer);
  LOAD(av_frame_move_ref);
  LOAD(av_frame_copy_props);
#if LIBAVCODEC_VERSION_MAJOR >= 62
  LOAD(av_frame_side_data_remove_by_props);
#else
  LOAD(av_frame_remove_side_data);
#endif
#if LIBAVCODEC_VERSION_MAJOR >= 60
  LOAD(av_channel_layout_index_from_channel);
  LOAD(av_channel_layout_from_mask);
  LOAD(av_channel_layout_copy);
#if LIBAVCODEC_VERSION_MAJOR >= 62
  LOAD(av_channel_layout_custom_init);
#else
  LOAD(av_calloc);
#endif
#endif
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
#undef LOAD
  stock_av_packet_unref=reinterpret_cast<decltype(stock_av_packet_unref)>(module_symbol(codec,"av_packet_unref"));
  stock_av_packet_get_side_data=reinterpret_cast<decltype(stock_av_packet_get_side_data)>(
    module_symbol(codec,"av_packet_get_side_data"));
  stock_av_packet_new_side_data=reinterpret_cast<decltype(stock_av_packet_new_side_data)>(
    module_symbol(codec,"av_packet_new_side_data"));
  return stock_av_packet_unref && stock_av_packet_get_side_data && stock_av_packet_new_side_data;
}

bool install(NativeTimingSlots settings) {
  std::lock_guard<std::mutex> lock(install_mutex);
  if(stop_requested)return false;
  if(installed_image) {
    if(!accepting || installed_image!=settings.image)return false;
    if(settings.open_return) {
      if(!audio_ready)return false;
      const auto* caller=reinterpret_cast<const unsigned char*>(settings.image)+settings.open_return;
      if(open_call_return && open_call_return!=caller)return false;
      open_call_return=caller;
    }
    return true;
  }
  std::array<LoadedModule,3> libraries{{
    {retain_loaded_module("avformat",LIBAVFORMAT_VERSION_MAJOR),release_module},
    {retain_loaded_module("avcodec",LIBAVCODEC_VERSION_MAJOR),release_module},
    {retain_loaded_module("avutil",LIBAVUTIL_VERSION_MAJOR),release_module},
  }};
  const auto format=libraries[0].get(),codec=libraries[1].get(),util=libraries[2].get();
  if(!format || !codec || !util)return false;
  auto* image=reinterpret_cast<unsigned char*>(settings.image);
#ifdef _WIN32
  const bool find_dispatch=!settings.format_open;
  if(find_dispatch) {
    // Establish that the video module owns this ABI's hardware dispatch
    // before considering stricter packet, frame and demux slot checks below.
    for(const char* symbol:{"av_hwframe_transfer_data","av_hwdevice_ctx_create"}) {
      auto* address=module_symbol(util,symbol);
      if(!address || !find_video_dispatch_slot(image,address))return false;
    }
  }
#endif
#if defined(_WIN32) && LIBAVCODEC_VERSION_MAJOR >= 62
  auto* allocator=module_symbol(util,"av_malloc");
  if(!allocator)return false;
  if(find_dispatch) {
    if(!find_video_dispatch_slot(image,allocator))return false;
  } else if(*reinterpret_cast<void**>(image+settings.format_open+0x80)!=allocator)return false;
#elif defined(__APPLE__) && LIBAVCODEC_VERSION_MAJOR >= 62
  auto* allocator=module_symbol(util,"av_malloc");
  if(!allocator)return false;
  if(!find_data_slot(reinterpret_cast<const mach_header_64*>(image),allocator))return false;
#endif

  stock_av_rescale_q=reinterpret_cast<decltype(stock_av_rescale_q)>(module_symbol(util,"av_rescale_q"));
  if(!stock_av_rescale_q)return false;
  stock_av_packet_clone=reinterpret_cast<decltype(stock_av_packet_clone)>(module_symbol(codec,"av_packet_clone"));
  stock_av_packet_free=reinterpret_cast<decltype(stock_av_packet_free)>(module_symbol(codec,"av_packet_free"));
  if(!stock_av_packet_clone || !stock_av_packet_free)return false;
#ifdef FRZZ_HDR_LEGACY_METADATA
  stock_av_buffer_ref=reinterpret_cast<decltype(stock_av_buffer_ref)>(module_symbol(util,"av_buffer_ref"));
  stock_av_buffer_unref=reinterpret_cast<decltype(stock_av_buffer_unref)>(module_symbol(util,"av_buffer_unref"));
  if(!stock_av_buffer_ref || !stock_av_buffer_unref)return false;
#endif
  std::size_t count=0;
#ifdef _WIN32
  const auto lookup_slot=[&](std::size_t offset,void* address) {
    if(find_dispatch)return find_video_dispatch_slot(image,address);
    auto** slot=reinterpret_cast<void**>(image+settings.format_open+offset);
    return *slot==address?slot:find_video_dispatch_slot(image,address);
  };
#define SLOT(offset,address) lookup_slot(offset,address)
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
  const auto prepare_demux=[&]() -> bool {
    PATCH(avformat_open_input,format,0x0,open_input);
    PATCH(avformat_find_stream_info,format,0x10,find_stream_info);
    PATCH(avcodec_parameters_to_context,codec,0x118,parameters_to_context);
    return true;
  };
  const auto prepare_audio=[&]() -> bool {
#if LIBAVCODEC_VERSION_MAJOR < 62
    auto* frame_alloc=module_symbol(util,"av_frame_alloc");
    if(!frame_alloc || !SLOT(0,frame_alloc))return false;
#endif
    return bind_audio(util,codec) && prepare_demux();
  };
  const bool can_route_audio=
#ifdef _WIN32
    !find_dispatch &&
#endif
    prepare_audio();
  if(!can_route_audio)count=0;
  if(settings.open_return && !can_route_audio)return false;
#ifdef _WIN32
  if(find_dispatch && !prepare_demux())return false;
#endif
  PATCH(avformat_close_input,format,0x18,close_input);
  PATCH(av_read_frame,format,0x20,read_frame);
  PATCH(avformat_seek_file,format,0x90,seek_file);
  PATCH(avcodec_send_packet,codec,0xa8,send_packet);
  PATCH(avcodec_receive_frame,codec,0xb0,receive_frame);
  PATCH(avcodec_flush_buffers,codec,0xc0,flush_buffers);
#if LIBAVCODEC_VERSION_MAJOR < 62
  auto* free_address=module_symbol(codec,"avcodec_free_context");
  auto* close_address=module_symbol(codec,"avcodec_close");
  const bool uses_free=free_address && SLOT(0xf8,free_address);
  const bool uses_close=close_address && SLOT(0,close_address);
  if(!uses_free && !uses_close)return false;
  if(uses_free) {
    PATCH(avcodec_free_context,codec,0xf8,free_context);
  }
  if(uses_close) {
    PATCH(avcodec_close,codec,0,close_codec);
  }
#else
  PATCH(avcodec_free_context,codec,0xf8,free_context);
#endif
#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
  bool hdr_ready=false;
#endif
#ifdef FRZZ_MACOS_HDR
  if(auto* address=module_symbol(util,"av_hwframe_transfer_data")) {
    if(auto** slot=SLOT(0,address);slot && hdr_destination.bind(util) && initialize_macos_hdr(util)) {
      if(!can_route_audio) {
        const auto video_only_count=count;
        if(!prepare_demux())count=video_only_count;
      }
      stock_av_hwframe_transfer_data=reinterpret_cast<decltype(stock_av_hwframe_transfer_data)>(address);
      patches[count++]={slot,address,reinterpret_cast<void*>(transfer_hardware_frame)};
      hdr_ready=true;
    }
  }
#elif defined(FRZZ_WINDOWS_HDR)
  if(auto* address=module_symbol(util,"av_hwframe_transfer_data")) {
    if(auto** slot=find_video_dispatch_slot(image,address);slot && hdr_destination.bind(util) && initialize_windows_hdr(util)) {
      if(!can_route_audio && !find_dispatch) {
        const auto video_only_count=count;
        if(!prepare_demux())count=video_only_count;
      }
      stock_av_hwframe_transfer_data=reinterpret_cast<decltype(stock_av_hwframe_transfer_data)>(address);
      patches[count++]={slot,address,reinterpret_cast<void*>(transfer_hardware_frame)};
      hdr_ready=true;
    }
  }
  if(hdr_ready) {
    if(auto* is_decoder=module_symbol(codec,"av_codec_is_decoder")) {
      if(auto* address=module_symbol(codec,"avcodec_open2")) {
        if(auto** slot=find_video_dispatch_slot(image,address)) {
          stock_av_codec_is_decoder=reinterpret_cast<decltype(stock_av_codec_is_decoder)>(is_decoder);
          stock_avcodec_open2=reinterpret_cast<decltype(stock_avcodec_open2)>(address);
          patches[count++]={slot,address,reinterpret_cast<void*>(open_hardware_codec)};
        }
      }
    }
    if(auto* address=module_symbol(util,"av_hwdevice_ctx_create")) {
      if(auto** slot=find_video_dispatch_slot(image,address)) {
        stock_av_hwdevice_ctx_create=reinterpret_cast<decltype(stock_av_hwdevice_ctx_create)>(address);
        patches[count++]={slot,address,reinterpret_cast<void*>(create_hardware_device)};
      }
    }
  }

#endif
#undef PATCH
#undef SLOT

#ifdef _WIN32
  if(find_dispatch) {
    HMODULE video{},self{};
    constexpr DWORD flags=GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN;
    if(!GetModuleHandleExW(flags,reinterpret_cast<LPCWSTR>(&install),&self) ||
       !GetModuleHandleExW(flags,reinterpret_cast<LPCWSTR>(image),&video) || video!=settings.image)return false;
  }
#endif
  open_call_return=settings.open_return?image+settings.open_return:nullptr;
  std::size_t written=0;
  for(;written<count;++written) {
    const auto& patch=patches[written];
    if(!exchange_slot(patch.slot,patch.previous,patch.replacement))break;
  }
  // A host thread can still be returning through a published handler after
  // rollback or shutdown. Keep its original FFmpeg exports loaded as well.
  if(written)for(auto& library:libraries)library.release();
  if(written!=count) {
    for(std::size_t index=0;index<written;++index) {
      const auto& patch=patches[index];
      exchange_slot(patch.slot,patch.replacement,patch.previous);
    }
    // Leave the bindings intact for callbacks that saw the partial update.
    if(written)stop_requested=true;
    return false;
  }
#if defined(FRZZ_MACOS_HDR) || defined(FRZZ_WINDOWS_HDR)
  hdr_installed=hdr_ready;
#endif
  enabled=true;
  accepting=true;
  audio_ready=can_route_audio;
  installed_patches=written;
  installed_image=settings.image;
  return true;
}

void finish_shutdown() noexcept {
  if(accepting || !enabled)return;
  std::lock_guard<std::mutex> lock(install_mutex);
  if(accepting || !enabled)return;
  {
    std::lock_guard<std::shared_mutex> state_lock(state_mutex);
    if(!inputs.empty() || !decoders.empty())return;
    if(!video_packets.empty())return;
    video_seeks.clear();
    preroll_packets.clear();
    enabled=false;
  }
  for(std::size_t i=0;i<installed_patches;++i) {
    const auto& patch=patches[i];
    exchange_slot(patch.slot,patch.replacement,patch.previous);
  }
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
#ifdef FRZZ_HDR_LEGACY_METADATA
  {
    std::lock_guard<std::mutex> lock(hdr_metadata_mutex);
    late_hdr_packets.clear();
    late_hdr_pending=false;
  }
#endif
#ifdef FRZZ_MACOS_HDR
  finish_macos_hdr_preparation();
#elif defined(FRZZ_WINDOWS_HDR)
  stop_windows_hdr_preparation();
#endif
  finish_shutdown();
}
}

extern const NativeTimingHook FRZZ_NATIVE_TIMING{
  LIBAVFORMAT_VERSION_MAJOR,install,FRZZ_CONTAINER_PLANAR_BED,select,enable_channel_projection,shutdown
};
}
#endif

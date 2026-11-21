#include "container_decoder.h"
#include "matroska_eac3.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

namespace frzz {
namespace {
#define FIELD(name) decltype(&name) name##_fn{}
struct Input {
  AVFormatContext* ctx{};
  std::uint64_t deadline{};
  FIELD(avformat_alloc_context);
  FIELD(avformat_open_input);
  FIELD(avformat_close_input);
  FIELD(avformat_find_stream_info);
  FIELD(av_read_frame);
  FIELD(av_seek_frame);
  FIELD(av_dict_set);
  FIELD(av_dict_free);
  FIELD(av_dict_get);
  FIELD(avcodec_get_name);
  FIELD(avcodec_find_decoder);

  ~Input(){if(ctx)avformat_close_input_fn(&ctx);}
  bool open(const char* path,const ContainerModules& modules,bool inspect) {
    if(ctx)avformat_close_input_fn(&ctx);
#define LOAD(name,module) name##_fn=reinterpret_cast<decltype(name##_fn)>(module_symbol(modules.module,#name)); if(!name##_fn)return false
    LOAD(avformat_alloc_context,format);
    LOAD(avformat_open_input,format);
    LOAD(avformat_close_input,format);
    LOAD(avformat_find_stream_info,format);
    LOAD(av_read_frame,format);
    LOAD(av_seek_frame,format);
    LOAD(av_dict_set,util);
    LOAD(av_dict_free,util);
    LOAD(av_dict_get,util);
    LOAD(avcodec_get_name,codec);
    LOAD(avcodec_find_decoder,codec);
    ctx=avformat_alloc_context_fn();
    if(!ctx)return false;
    deadline=monotonic_ms()+4000;
    ctx->interrupt_callback={[](void* user)->int {
      return monotonic_ms()>static_cast<Input*>(user)->deadline;
    },this};
    AVDictionary* options=nullptr;
    av_dict_set_fn(&options,"protocol_whitelist","file",0);
    const int status=avformat_open_input_fn(&ctx,path,nullptr,&options);
    av_dict_free_fn(&options);
    if(status<0)return false;
    return !inspect || avformat_find_stream_info_fn(ctx,nullptr)>=0;
  }
  std::string tag(const AVStream* stream,const char* key) const {
    const auto* value=av_dict_get_fn(stream->metadata,key,nullptr,0);
    return value?value->value:"";
  }
  double origin() const {
    return ctx->start_time!=AV_NOPTS_VALUE?static_cast<double>(ctx->start_time)/AV_TIME_BASE:0;
  }
};

template<class Audio>
int channels(const Audio* audio) {
#if LIBAVCODEC_VERSION_MAJOR == 59
  return audio->channels;
#else
  return audio->ch_layout.nb_channels;
#endif
}

const char* speaker_label(AVChannel channel,bool has_side_surrounds) {
  switch(channel) {
    case AV_CHAN_FRONT_LEFT: return "L";
    case AV_CHAN_FRONT_RIGHT: return "R";
    case AV_CHAN_FRONT_CENTER: return "C";
    case AV_CHAN_LOW_FREQUENCY: return "LFE";
    case AV_CHAN_SIDE_LEFT: return "Ls";
    case AV_CHAN_SIDE_RIGHT: return "Rs";
    case AV_CHAN_BACK_LEFT: return has_side_surrounds?"Lrs":"Ls";
    case AV_CHAN_BACK_RIGHT: return has_side_surrounds?"Rrs":"Rs";
    case AV_CHAN_FRONT_LEFT_OF_CENTER: return "Lc";
    case AV_CHAN_FRONT_RIGHT_OF_CENTER: return "Rc";
    case AV_CHAN_WIDE_LEFT: return "Lw";
    case AV_CHAN_WIDE_RIGHT: return "Rw";
    case AV_CHAN_LOW_FREQUENCY_2: return "LFE2";
    default: return nullptr;
  }
}

template<class Audio>
std::vector<AVChannel> speaker_positions(const Audio* audio,ModuleHandle util) {
  std::vector<AVChannel> speakers;
#if LIBAVCODEC_VERSION_MAJOR == 59
  (void)util;
  for(unsigned bit=0;bit<64;++bit)
    if(audio->channel_layout&(std::uint64_t{1}<<bit))speakers.push_back(static_cast<AVChannel>(bit));
#else
  const auto at=reinterpret_cast<decltype(&av_channel_layout_channel_from_index)>(module_symbol(util,"av_channel_layout_channel_from_index"));
  const auto& layout=audio->ch_layout;
  if(!at || layout.order==AV_CHANNEL_ORDER_UNSPEC || layout.nb_channels>128)return {};
  for(int i=0;i<layout.nb_channels;++i)speakers.push_back(at(&layout,static_cast<unsigned>(i)));
#endif
  if(static_cast<int>(speakers.size())!=channels(audio))return {};
  return speakers;
}

bool is_height(AVChannel speaker) {
  if(speaker>=AV_CHAN_TOP_CENTER && speaker<=AV_CHAN_TOP_BACK_RIGHT)return true;
#ifdef AV_CH_TOP_SURROUND_LEFT
  if(speaker==AV_CHAN_TOP_SURROUND_LEFT || speaker==AV_CHAN_TOP_SURROUND_RIGHT)return true;
#endif
  return speaker==AV_CHAN_TOP_SIDE_LEFT || speaker==AV_CHAN_TOP_SIDE_RIGHT;
}

std::string channel_order(const std::vector<AVChannel>& speakers,ModuleHandle util) {
#if LIBAVCODEC_VERSION_MAJOR == 59
  const auto describe=reinterpret_cast<decltype(&av_get_channel_description)>(module_symbol(util,"av_get_channel_description"));
#else
  const auto describe=reinterpret_cast<decltype(&av_channel_description)>(module_symbol(util,"av_channel_description"));
#endif
  const bool has_side_surrounds=std::any_of(speakers.begin(),speakers.end(),[](AVChannel speaker) {
    return speaker==AV_CHAN_SIDE_LEFT || speaker==AV_CHAN_SIDE_RIGHT;
  });
  std::string order;
  for(const auto channel:speakers) {
    if(!order.empty())order+=' ';
    const char* label=speaker_label(channel,has_side_surrounds);
    if(label){order+=label;continue;}
#if LIBAVCODEC_VERSION_MAJOR == 59
    label=describe?describe(std::uint64_t{1}<<static_cast<unsigned>(channel)):nullptr;
    order+=label && *label?label:"?";
#else
    char description[80]{};
    if(channel!=AV_CHAN_NONE && channel!=AV_CHAN_UNKNOWN && describe && describe(description,sizeof(description),channel)>=0)
      order+=description;
    else order+='?';
#endif
  }
  return order;
}

#if LIBAVCODEC_VERSION_MAJOR == 62 || LIBAVCODEC_VERSION_MAJOR == 63
bool native_bed_sample_format(int format) {
  return format==AV_SAMPLE_FMT_S16P || format==AV_SAMPLE_FMT_S32P || format==AV_SAMPLE_FMT_FLTP;
}
#endif

void select_main_channels(const AVCodecParameters* params,ModuleHandle util,ContainerStream& stream) {
  stream.source_channels=stream.channels=channels(params);
  const auto positions=speaker_positions(params,util);
  if(positions.empty())return;

  std::vector<AVChannel> bed;
  for(size_t i=0;i<positions.size();++i) {
    if(is_height(positions[i]))continue;
    bed.push_back(positions[i]);
    stream.channel_map.push_back(static_cast<int>(i));
  }
  stream.channels=static_cast<int>(bed.size());
  if(stream.channels==stream.source_channels)stream.channel_map.clear();
  else stream.bed_speakers.assign(bed.begin(),bed.end());
#if LIBAVCODEC_VERSION_MAJOR == 62 || LIBAVCODEC_VERSION_MAJOR == 63
  stream.native_planar_bed=!stream.channel_map.empty() && native_bed_sample_format(params->format) &&
    params->ch_layout.order==AV_CHANNEL_ORDER_NATIVE;
#endif
  stream.channel_order=channel_order(bed,util);
}

std::int64_t timestamp_frames(double seconds,int rate) {
  const double frames=seconds*rate;
  if(!std::isfinite(frames) || std::abs(frames)>=static_cast<double>(INT64_MAX))return 0;
  return std::llround(frames);
}

double sample(const AVFrame& frame,int ch,int i,int nch) {
  switch(frame.format) {
    case AV_SAMPLE_FMT_U8: return (frame.extended_data[0][i*nch+ch]-128)/128.0;
    case AV_SAMPLE_FMT_U8P: return (frame.extended_data[ch][i]-128)/128.0;
    case AV_SAMPLE_FMT_S16: return reinterpret_cast<const int16_t*>(frame.extended_data[0])[i*nch+ch]/32768.0;
    case AV_SAMPLE_FMT_S16P: return reinterpret_cast<const int16_t*>(frame.extended_data[ch])[i]/32768.0;
    case AV_SAMPLE_FMT_S32: return reinterpret_cast<const int32_t*>(frame.extended_data[0])[i*nch+ch]/2147483648.0;
    case AV_SAMPLE_FMT_S32P: return reinterpret_cast<const int32_t*>(frame.extended_data[ch])[i]/2147483648.0;
    case AV_SAMPLE_FMT_S64: return static_cast<double>(reinterpret_cast<const int64_t*>(frame.extended_data[0])[i*nch+ch])/9223372036854775808.0;
    case AV_SAMPLE_FMT_S64P: return static_cast<double>(reinterpret_cast<const int64_t*>(frame.extended_data[ch])[i])/9223372036854775808.0;
    case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float*>(frame.extended_data[0])[i*nch+ch];
    case AV_SAMPLE_FMT_FLTP: return reinterpret_cast<const float*>(frame.extended_data[ch])[i];
    case AV_SAMPLE_FMT_DBL: return reinterpret_cast<const double*>(frame.extended_data[0])[i*nch+ch];
    case AV_SAMPLE_FMT_DBLP: return reinterpret_cast<const double*>(frame.extended_data[ch])[i];
    default:return 0;
  }
}

class Decoder final:public ContainerDecoder {
  std::shared_ptr<ContainerModules> modules;
  Input input;
  ContainerStream stream;
  std::string file;
  AVCodecContext* codec{};
  AVCodecParameters* parameters{};
  const AVCodec* implementation{};
  AVPacket* packet{};
  AVFrame* frame{};
  std::vector<AVChannel> source_layout;
  std::vector<int> frame_channel_map;
  AVRational clock{};
  int seek_video=-1;
  std::optional<int64_t> pending_anchor;
  bool native_timestamps{};
  bool coarse_eac3{},matroska{};
  unsigned eac3_streams{};
  int eac3_probe_frames{};
  int64_t native_timestamp_offset{};
  bool restart_for_cadence{};
  int64_t position{}, frame_start{}, frame_end{};
  bool have_frame{}, input_done{}, have_time{},failed{},anchored=true;
  FIELD(avcodec_alloc_context3);
  FIELD(avcodec_parameters_alloc);
  FIELD(avcodec_parameters_copy);
  FIELD(avcodec_parameters_free);
  FIELD(avcodec_parameters_to_context);
  FIELD(avcodec_open2);
  FIELD(avcodec_free_context);
  FIELD(avcodec_send_packet);
  FIELD(avcodec_receive_frame);
  FIELD(av_packet_alloc);
  FIELD(av_packet_unref);
  FIELD(av_packet_free);
  FIELD(av_frame_alloc);
  FIELD(av_frame_unref);
  FIELD(av_frame_free);

  bool reset_codec() {
    if(codec)avcodec_free_context_fn(&codec);
    codec=avcodec_alloc_context3_fn(implementation);
    if(!codec || avcodec_parameters_to_context_fn(codec,parameters)<0)return false;
    codec->pkt_timebase=clock;
    AVDictionary* options=nullptr;
    if(parameters->codec_id==AV_CODEC_ID_AC3 || parameters->codec_id==AV_CODEC_ID_EAC3)
      input.av_dict_set_fn(&options,"cons_noisegen","1",0);
    const auto result=avcodec_open2_fn(codec,implementation,&options);
    input.av_dict_free_fn(&options);
    return result>=0;
  }

  bool frame_fits_timestamp_grid(int samples) const {
    return (int64_t(samples)*clock.den)%(int64_t(stream.rate)*clock.num)==0;
  }

  bool can_seek_on_timestamp_grid() {
    if(!native_timestamps)return false;
    if(!coarse_eac3)return true;
    if(!matroska)return false;
    std::lock_guard<std::mutex> lock(stream.timeline->eac3_grid_mutex);
    auto& verified=stream.timeline->eac3_grid;
    if(!verified.has_value())verified=matroska_eac3_on_timestamp_grid(file.c_str(),clock.num,clock.den,eac3_streams);
    return *verified;
  }

  bool seek_input(double seconds,double audio_timestamp) {
    if(seek_video>=0 && static_cast<unsigned>(seek_video)<input.ctx->nb_streams) {
      auto* video=input.ctx->streams[seek_video];
      const double ts=seconds/av_q2d(video->time_base);
      if(std::isfinite(ts) && std::abs(ts)<double(INT64_MAX)) {
        // Matroska cues are commonly indexed only for video. Seeking via the
        // selected audio track can instead scan large portions of the file.
        const auto discard=video->discard;
        video->discard=AVDISCARD_DEFAULT;
        const auto result=input.av_seek_frame_fn(input.ctx,seek_video,static_cast<int64_t>(std::floor(ts)),AVSEEK_FLAG_BACKWARD);
        video->discard=discard;
        if(result>=0)return true;
      }
    }
    if(input.av_seek_frame_fn(input.ctx,stream.index,static_cast<int64_t>(std::floor(audio_timestamp)),AVSEEK_FLAG_BACKWARD)>=0)
      return true;
    const double global=seconds*AV_TIME_BASE;
    return std::abs(global)<double(INT64_MAX) &&
      input.av_seek_frame_fn(input.ctx,-1,static_cast<int64_t>(std::floor(global)),AVSEEK_FLAG_BACKWARD)>=0;
  }

  bool seek(int64_t target,bool from_start=false) {
    input.deadline=monotonic_ms()+4000;
    const auto preroll=2LL*stream.rate;
    const auto requested=std::max<int64_t>(0,target-preroll);
    auto begin=requested;
    pending_anchor.reset();
    bool direct_grid=false;
    if(stream.timeline) {
      // Coarse timestamps alone cannot determine the PCM phase after a gap.
      // Prefer a nearby verified frame, then a proven native clock, then a linear read.
      const double ts=(double(requested)/stream.rate+stream.origin)/av_q2d(clock);
      if(!from_start && std::isfinite(ts) && std::abs(ts)<double(INT64_MAX)) {
        std::lock_guard<std::mutex> lock(stream.timeline->mutex);
        const auto& anchors=stream.timeline->anchors;
        const auto after=anchors.upper_bound(static_cast<int64_t>(std::floor(ts)));
        if(after!=anchors.begin()) {
          const auto anchor=std::prev(after);
          if(anchor->second.frame>preroll && anchor->second.frame<=requested) {
            begin=anchor->second.frame-preroll;
            pending_anchor=anchor->first;
          }
        }
      }
      const bool nearby=pending_anchor && target-begin<=10LL*stream.rate;
      if(!from_start && !nearby && requested && (!coarse_eac3 || target>10LL*stream.rate) &&
         can_seek_on_timestamp_grid()) {
        begin=requested;
        direct_grid=true;
        pending_anchor.reset();
      } else if(!pending_anchor) {
        begin=0;
      }
      if(coarse_eac3 && !direct_grid) {
        native_timestamps=false;
        native_timestamp_offset=0;
        eac3_probe_frames=begin==0?2:0;
      }
    }
    input.deadline=monotonic_ms()+4000;
    const double seconds=static_cast<double>(begin)/stream.rate+stream.origin;
    const double ts=seconds/av_q2d(clock);
    if(!std::isfinite(ts) || std::abs(ts)>=static_cast<double>(INT64_MAX))return false;
    if(begin==0 || !input.ctx) {
      if(!input.open(file.c_str(),*modules,false))return false;
      discard_other_streams();
    }
    if(begin>0 && !seek_input(seconds,ts))return false;
    if(!reset_codec())return false;
    av_frame_unref_fn(frame);
    have_frame=input_done=have_time=failed=false;
    anchored=begin==0;
    frame_start=frame_end=begin;
    return true;
  }

  void discard_other_streams() {
    for(unsigned i=0;i<input.ctx->nb_streams;++i)
      if(static_cast<int>(i)!=stream.index)input.ctx->streams[i]->discard=AVDISCARD_ALL;
  }

  bool next_frame() {
    av_frame_unref_fn(frame);
    for(;;) {
      const int received=avcodec_receive_frame_fn(codec,frame);
      if(received>=0)break;
      if(received!=AVERROR(EAGAIN) || input_done) {
        failed=!input_done || (received!=AVERROR_EOF && received!=AVERROR(EAGAIN));
        return false;
      }
      int status;
      input.deadline=monotonic_ms()+4000;
      while((status=input.av_read_frame_fn(input.ctx,packet))>=0 && packet->stream_index!=stream.index)
        av_packet_unref_fn(packet);
      if(status<0) {
        if(status!=AVERROR_EOF){failed=true;return false;}
        input_done=true;
        if(avcodec_send_packet_fn(codec,nullptr)<0){failed=true;return false;}
      } else {
        const int sent=avcodec_send_packet_fn(codec,packet);
        av_packet_unref_fn(packet);
        if(sent<0){failed=true;return false;}
      }
    }
    if(channels(frame)!=stream.source_channels || frame->sample_rate!=stream.rate || frame->nb_samples<=0 || !frame->extended_data) {
      failed=true;
      return false;
    }
    if(native_timestamps && !pending_anchor && have_time && !frame_fits_timestamp_grid(frame->nb_samples)) {
      native_timestamps=false;
      if(!anchored) {
        restart_for_cadence=true;
        return false;
      }
    }
    if(!stream.channel_map.empty()) {
      // Resolve the retained speakers again if the decoder reports a different order.
      const auto positions=speaker_positions(frame,modules->util);
      frame_channel_map=stream.channel_map;
      if(!positions.empty() && positions!=source_layout) {
        for(auto& index:frame_channel_map) {
          const auto match=std::find(positions.begin(),positions.end(),source_layout[static_cast<size_t>(index)]);
          if(match==positions.end()){failed=true;return false;}
          index=static_cast<int>(match-positions.begin());
        }
      }
    }
    const int64_t ts=frame->best_effort_timestamp!=AV_NOPTS_VALUE?frame->best_effort_timestamp:frame->pts;
    const int64_t expected=frame_end;
    const auto timestamp_start=ts==AV_NOPTS_VALUE?expected:
      timestamp_frames(ts*av_q2d(clock)-stream.origin,stream.rate);
    frame_start=timestamp_start;
    if(native_timestamps && ts!=AV_NOPTS_VALUE)frame_start+=native_timestamp_offset;
    const auto tolerance=static_cast<int64_t>(std::ceil(av_q2d(clock)*stream.rate));
    if(!native_timestamps && have_time && std::abs(static_cast<double>(frame_start)-expected)<=static_cast<double>(tolerance))
      frame_start=expected;
    if(stream.timeline && ts!=AV_NOPTS_VALUE) {
      std::lock_guard<std::mutex> lock(stream.timeline->mutex);
      auto& anchors=stream.timeline->anchors;
      const auto found=anchors.lower_bound(ts);
      if(found!=anchors.end() && found->first==ts) {
        frame_start=found->second.frame;
        native_timestamps=found->second.native_timestamps;
        native_timestamp_offset=found->second.timestamp_offset;
        eac3_probe_frames=found->second.cadence_probe_frames;
        anchored=true;
        pending_anchor.reset();
      } else {
        if(eac3_probe_frames>0 && --eac3_probe_frames==0 && frame_fits_timestamp_grid(frame->nb_samples)) {
          native_timestamps=true;
          native_timestamp_offset=frame_start-timestamp_start;
        }
        if(anchored && anchors.size()<43200) {
          const double second=1.0/av_q2d(clock);
          const bool before=found==anchors.begin() || double(ts)-std::prev(found)->first>=second;
          const bool after=found==anchors.end() || double(found->first)-ts>=second;
          if(before && after)
            anchors.emplace_hint(found,ts,ContainerTimeline::Anchor{frame_start,native_timestamp_offset,eac3_probe_frames,native_timestamps});
        }
      }
    } else if(eac3_probe_frames>0) {
      --eac3_probe_frames;
    }
    if(frame_start>INT64_MAX-frame->nb_samples)return false;
    frame_end=frame_start+frame->nb_samples;
    have_time=true;
    return true;
  }
public:
  Decoder(std::shared_ptr<ContainerModules> bound,const ContainerStream& info):modules(std::move(bound)),stream(info){}
  ~Decoder() override {
    if(frame)av_frame_free_fn(&frame);
    if(packet)av_packet_free_fn(&packet);
    if(codec)avcodec_free_context_fn(&codec);
    if(parameters)avcodec_parameters_free_fn(&parameters);
  }
  bool open(const char* path) {
    file=path;
    const auto& libs=*modules;
#define DEC_LOAD(name,module) name##_fn=reinterpret_cast<decltype(name##_fn)>(module_symbol(libs.module,#name)); if(!name##_fn)return false
    DEC_LOAD(avcodec_alloc_context3,codec);
    DEC_LOAD(avcodec_parameters_alloc,codec);
    DEC_LOAD(avcodec_parameters_copy,codec);
    DEC_LOAD(avcodec_parameters_free,codec);
    DEC_LOAD(avcodec_parameters_to_context,codec);
    DEC_LOAD(avcodec_open2,codec);
    DEC_LOAD(avcodec_free_context,codec);
    DEC_LOAD(avcodec_send_packet,codec);
    DEC_LOAD(avcodec_receive_frame,codec);
    DEC_LOAD(av_packet_alloc,codec);
    DEC_LOAD(av_packet_unref,codec);
    DEC_LOAD(av_packet_free,codec);
    DEC_LOAD(av_frame_alloc,util);
    DEC_LOAD(av_frame_unref,util);
    DEC_LOAD(av_frame_free,util);
#undef DEC_LOAD
    if(!input.open(path,libs,true) || stream.index<0 || static_cast<unsigned>(stream.index)>=input.ctx->nb_streams)return false;
    auto* selected=input.ctx->streams[stream.index];
    auto* params=selected->codecpar;
    if(params->codec_type!=AVMEDIA_TYPE_AUDIO || channels(params)!=stream.source_channels || params->sample_rate!=stream.rate)return false;
    if(!stream.channel_map.empty()) {
      source_layout=speaker_positions(params,modules->util);
      ContainerStream current;
      select_main_channels(params,modules->util,current);
      if(current.channel_map!=stream.channel_map || current.channel_order!=stream.channel_order)return false;
    }
    clock=selected->time_base;
    if(clock.num<=0 || clock.den<=0)return false;
    matroska=std::string_view(input.ctx->iformat->name).substr(0,9)=="matroska,";
    if(matroska) {
      for(unsigned i=0;i<input.ctx->nb_streams;++i) {
        const auto* track=input.ctx->streams[i];
        if(track->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && !(track->disposition&AV_DISPOSITION_ATTACHED_PIC) &&
           track->time_base.num>0 && track->time_base.den>0) {
          seek_video=static_cast<int>(i);
          break;
        }
      }
    }
    // AC-3 is fixed at 1536 samples. E-AC-3 allows shorter frames, and
    // a demuxer can leave its codec parameter frame_size unset.
    native_timestamps=stream.timeline && params->codec_id==AV_CODEC_ID_AC3 && frame_fits_timestamp_grid(1536);
    // The first decoded frame may itself be shortened by codec delay.
    coarse_eac3=stream.timeline && params->codec_id==AV_CODEC_ID_EAC3;
    if(coarse_eac3) {
      eac3_probe_frames=2;
      for(unsigned i=0;i<input.ctx->nb_streams;++i)
        if(input.ctx->streams[i]->codecpar->codec_id==AV_CODEC_ID_EAC3)++eac3_streams;
    }
    discard_other_streams();
    implementation=input.avcodec_find_decoder_fn(params->codec_id);
    if(!implementation)return false;
    parameters=avcodec_parameters_alloc_fn();
    if(!parameters || avcodec_parameters_copy_fn(parameters,params)<0 || !reset_codec())return false;
    packet=av_packet_alloc_fn();
    frame=av_frame_alloc_fn();
    return packet && frame;
  }

  int read(int64_t target,double* output,int n) override {
    const auto nch=stream.channels;
    std::fill_n(output,static_cast<size_t>(n)*nch,0.0);
    if(position!=target) {
      while(eac3_probe_frames>0 && next_frame()) {}
      eac3_probe_frames=0;
    }
    if(position!=target && !seek(target) && (!stream.timeline || native_timestamps || !seek(target,true))) {
      position=-1;
      return 0;
    }
    input.deadline=monotonic_ms()+4000;
    const auto end=target+n;
    auto cursor=target;
    while(cursor<end) {
      if(!have_frame || frame_end<=cursor) {
        have_frame=next_frame();
        if(restart_for_cadence) {
          restart_for_cadence=false;
          if(!seek(cursor,true)){position=-1;return static_cast<int>(cursor-target);}
          continue;
        }
        while(have_frame && pending_anchor && frame_end<=cursor)
          have_frame=next_frame();
        if(pending_anchor) {
          if(!seek(target,true)){position=-1;return 0;}
          continue;
        }
        if(!have_frame)break;
        if(frame_end<=cursor)continue;
      }
      if(frame_start>=end)break;
      cursor=std::max(cursor,frame_start);
      const auto limit=std::min(end,frame_end);
      for(;cursor<limit;++cursor)
        for(int ch=0;ch<nch;++ch) {
          const auto input_channel=frame_channel_map.empty()?ch:frame_channel_map[static_cast<size_t>(ch)];
          output[(cursor-target)*nch+ch]=sample(*frame,input_channel,static_cast<int>(cursor-frame_start),stream.source_channels);
        }
    }
    if(failed){position=-1;return static_cast<int>(cursor-target);}
    position=end;
    return n;
  }
};
#undef FIELD
#undef LOAD
}

bool FRZZ_CONTAINER_PROBE(const char* path,const ContainerModules& modules,ContainerAudio& info) {
  Input input;
  if(!input.open(path,modules,true) || input.ctx->duration<=0)return false;
  info.duration=static_cast<double>(input.ctx->duration)/AV_TIME_BASE;
  info.quicktime=std::string_view(input.ctx->iformat->name).substr(0,4)=="mov,";
  for(unsigned i=0;i<input.ctx->nb_streams;++i) {
    const auto* stream=input.ctx->streams[i];
    const auto* params=stream->codecpar;
    if(params->codec_type==AVMEDIA_TYPE_VIDEO && !(stream->disposition&AV_DISPOSITION_ATTACHED_PIC))info.has_video=true;
    if(params->codec_type!=AVMEDIA_TYPE_AUDIO)continue;
    ContainerStream entry;
    entry.default_track=(stream->disposition&AV_DISPOSITION_DEFAULT)!=0;
    entry.index=static_cast<int>(i);
    entry.track_id=static_cast<std::uint32_t>(stream->id);
    entry.rate=params->sample_rate;
    select_main_channels(params,modules.util,entry);
    entry.origin=input.origin();
    const bool has_clock=stream->time_base.num>0 && stream->time_base.den>0;
    const auto begin=has_clock && stream->start_time!=AV_NOPTS_VALUE?
      stream->start_time*av_q2d(stream->time_base):entry.origin;
    entry.start_offset_frames=timestamp_frames(begin-entry.origin,entry.rate);
    if(stream->time_base.num>0 && stream->time_base.den>0 && av_q2d(stream->time_base)*entry.rate>1.01)
      entry.timeline=std::make_shared<ContainerTimeline>();
    entry.language=input.tag(stream,"language");
    entry.title=input.tag(stream,"title");
    if(entry.title.empty()) {
      const auto handler=input.tag(stream,"handler_name");
      if(handler!="SoundHandler")entry.title=handler;
    }
    entry.codec=input.avcodec_get_name_fn(params->codec_id);
    double end=info.duration;
    if(stream->duration>0 && has_clock)
      end=begin+stream->duration*av_q2d(stream->time_base)-entry.origin;
    entry.frames=timestamp_frames(std::max(end,info.duration),entry.rate);
    if(entry.channels<=0 || entry.source_channels>128 || entry.rate<=0 || !input.avcodec_find_decoder_fn(params->codec_id))entry.frames=0;
    info.streams.push_back(std::move(entry));
  }
  return true;
}

ContainerDecoder* FRZZ_CONTAINER_OPEN(const char* path,std::shared_ptr<ContainerModules> modules,const ContainerStream& info) {
  auto decoder=std::make_unique<Decoder>(std::move(modules),info);
  return decoder->open(path)?decoder.release():nullptr;
}

#if LIBAVCODEC_VERSION_MAJOR == 62 || LIBAVCODEC_VERSION_MAJOR == 63
bool FRZZ_CONTAINER_PLANAR_BED(const void* codec_context,int source_channels,int main_channels) noexcept {
  if(!codec_context || main_channels<=0 || main_channels>8 || source_channels<=main_channels)return false;
  const auto& codec=*static_cast<const AVCodecContext*>(codec_context);
  if(codec.codec_type!=AVMEDIA_TYPE_AUDIO || !native_bed_sample_format(codec.sample_fmt) ||
     codec.ch_layout.order!=AV_CHANNEL_ORDER_NATIVE || codec.ch_layout.nb_channels!=source_channels)return false;
  // REAPER can read a prefix of planar audio without changing FFmpeg's
  // decoder or touching the decoded frame. Packed audio needs its full stride.
  int channel=0;
  for(unsigned bit=0;bit<64;++bit) {
    if(!(codec.ch_layout.u.mask&(std::uint64_t{1}<<bit)))continue;
    if(is_height(static_cast<AVChannel>(bit))!=(channel>=main_channels))return false;
    ++channel;
  }
  return channel==source_channels;
}
#endif
}

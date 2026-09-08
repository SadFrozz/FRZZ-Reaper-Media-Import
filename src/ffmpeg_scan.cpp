#include "media_metadata.h"
#include <cmath>
#include <limits>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/packet.h>
}
namespace frzz {
bool FRZZ_FFMPEG_SCAN(const char* path,ModuleHandle format_module,ModuleHandle codec_module,MediaMetadata& out) {
#define BIND(name,module) auto p_##name=reinterpret_cast<decltype(&name)>(module_symbol(module,#name)); if(!p_##name){out.error="Missing FFmpeg symbol " #name;return false;}
  BIND(avformat_version,format_module)
  BIND(avformat_alloc_context,format_module)
  BIND(avformat_open_input,format_module)
  BIND(avformat_find_stream_info,format_module)
  BIND(avformat_close_input,format_module)
  BIND(av_read_frame,format_module)
  BIND(av_packet_alloc,codec_module)
  BIND(av_packet_unref,codec_module)
  BIND(av_packet_free,codec_module)
  BIND(avcodec_get_name,codec_module)
#undef BIND
  if((p_avformat_version()>>16)!=LIBAVFORMAT_VERSION_MAJOR){out.error="FFmpeg ABI mismatch";return false;}
  AVFormatContext* ctx=p_avformat_alloc_context();
  if(!ctx){out.error="FFmpeg context allocation failed";return false;}
  struct Close {AVFormatContext*& p;decltype(p_avformat_close_input) close;~Close(){if(p)close(&p);}} close{ctx,p_avformat_close_input};
  const std::uint64_t deadline=monotonic_ms()+120000;
  ctx->interrupt_callback.callback=[](void* p)->int{return monotonic_ms()>*static_cast<const std::uint64_t*>(p);};
  ctx->interrupt_callback.opaque=const_cast<std::uint64_t*>(&deadline);
  if(p_avformat_open_input(&ctx,path,nullptr,nullptr)<0 || p_avformat_find_stream_info(ctx,nullptr)<0){out.error="FFmpeg could not identify input";return false;}
  int audio=-1;
  for(unsigned int i=0;i<ctx->nb_streams;++i)if(ctx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO){audio=static_cast<int>(i);break;}
  if(audio<0){out.error="No audio stream";return false;}
  auto stream=ctx->streams[audio];
  out.codec=p_avcodec_get_name(stream->codecpar->codec_id);
  if(!out.expected_codec.empty() && out.codec!=out.expected_codec){out.error="Detected codec "+out.codec+" differs from required "+out.expected_codec;return false;}
  out.rate=stream->codecpar->sample_rate;
#if LIBAVFORMAT_VERSION_MAJOR == 59
  // The legacy field is present throughout FFmpeg 5.x, including 5.0.
  out.channels=stream->codecpar->channels;
#else
  out.channels=stream->codecpar->ch_layout.nb_channels;
#endif
  if(out.rate<=0 || out.channels<=0 || stream->time_base.num<=0 || stream->time_base.den<=0){out.error="Invalid audio stream metadata";return false;}
  AVPacket* packet=p_av_packet_alloc();
  if(!packet){out.error="FFmpeg packet allocation failed";return false;}
  struct FreePacket {AVPacket*& p;decltype(p_av_packet_free) free;~FreePacket(){free(&p);}} free_packet{packet,p_av_packet_free};
  std::int64_t ticks=0;int result;
  while((result=p_av_read_frame(ctx,packet))>=0) {
    if(packet->stream_index==audio) {
      if(packet->duration<=0 || (packet->flags&AV_PKT_FLAG_CORRUPT) || packet->duration>std::numeric_limits<std::int64_t>::max()-ticks){out.error="Missing/invalid packet duration or corrupt packet";return false;}
      ticks+=packet->duration;++out.packets;
    }
    p_av_packet_unref(packet);
  }
  if(result!=AVERROR_EOF || !out.packets){out.error="Packet scan interrupted or incomplete";return false;}
  const long double frames=static_cast<long double>(ticks)*stream->time_base.num*out.rate/stream->time_base.den;
  if(frames<1 || frames>static_cast<long double>(std::numeric_limits<std::int64_t>::max())-1){out.error="Invalid sample-frame count";return false;}
  out.frames=static_cast<std::int64_t>(std::llround(frames));
  out.duration=static_cast<double>(out.frames)/out.rate;
  out.format_major=LIBAVFORMAT_VERSION_MAJOR;
  return std::isfinite(out.duration) && out.duration>0;
}
}

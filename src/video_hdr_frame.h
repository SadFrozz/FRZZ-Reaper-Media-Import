#pragma once

#include "platform.h"

#include <charconv>
#include <cstddef>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
}

namespace frzz {

namespace {

struct HdrStreamSideData {
  const AVPacketSideData* entries;
  int count;
};

inline HdrStreamSideData hdr_stream_side_data(const AVStream& stream) noexcept {
#if LIBAVCODEC_VERSION_MAJOR < 61
  return {stream.side_data,stream.nb_side_data};
#else
  return {stream.codecpar->coded_side_data,stream.codecpar->nb_coded_side_data};
#endif
}

inline AVDOVIDecoderConfigurationRecord hdr_dovi_config(const AVPacketSideData* entries, int count) noexcept {
  constexpr auto required=offsetof(AVDOVIDecoderConfigurationRecord,dv_bl_signal_compatibility_id)+sizeof(uint8_t);
  for(int i=0;i<count;++i) {
    const auto& entry=entries[i];
    if(entry.type!=AV_PKT_DATA_DOVI_CONF || !entry.data || entry.size<required)continue;
    AVDOVIDecoderConfigurationRecord config{};
    std::memcpy(&config,entry.data,required);
    return config;
  }
  return {};
}

class HdrFrameDovi {
public:
  bool bind(ModuleHandle util) noexcept {
    side_data_=reinterpret_cast<decltype(side_data_)>(module_symbol(util,"av_frame_get_side_data"));
    get_=reinterpret_cast<decltype(get_)>(module_symbol(util,"av_dict_get"));
    set_=reinterpret_cast<decltype(set_)>(module_symbol(util,"av_dict_set_int"));
    return side_data_ && get_ && set_;
  }

  void remember(const AVCodecContext& codec, AVFrame& frame,
                AVDOVIDecoderConfigurationRecord fallback={}) const noexcept {
    if(!side_data_ || !get_ || !set_)return;
    auto config=hdr_dovi_config(codec.coded_side_data,codec.nb_coded_side_data);
    if(!config.dv_profile)config=fallback;
    auto* rpu=side_data_(&frame,AV_FRAME_DATA_DOVI_METADATA);
    const bool has_rpu=has_base_rpu(rpu);
    if(!config.dv_profile && codec.codec_id==AV_CODEC_ID_HEVC && has_rpu) {
      const auto inferred=in_band_config(frame,*rpu);
      config.dv_profile=static_cast<uint8_t>(inferred.profile);
      config.dv_bl_signal_compatibility_id=static_cast<uint8_t>(inferred.compatibility);
    }
    if(config.dv_profile) {
      // Keep the profile even on frames that arrived without a usable RPU.
      // av_frame_ref copies both dictionaries without retaining another surface.
      auto** metadata=has_rpu?&rpu->metadata:&frame.metadata;
      set_(metadata,key,(config.dv_profile<<8)|config.dv_bl_signal_compatibility_id,0);
    }
  }

  struct Config {
    unsigned profile=0;
    unsigned compatibility=0;
    bool has_rpu=false;
  };

  Config get(const AVFrame& frame) const noexcept {
    if(!side_data_ || !get_ || !set_)return {};
    const auto* rpu=side_data_(&frame,AV_FRAME_DATA_DOVI_METADATA);
    const bool has_rpu=has_base_rpu(rpu);
    const auto* entry=get_(has_rpu?rpu->metadata:frame.metadata,key,nullptr,AV_DICT_MATCH_CASE);
    if(!entry)return {0,0,has_rpu};
    unsigned value=0;
    const auto* end=entry->value+std::strlen(entry->value);
    const auto result=std::from_chars(entry->value,end,value);
    if(result.ec!=std::errc{} || result.ptr!=end || value>0xffff)return {};
    return {value>>8,value&0xff,has_rpu};
  }

private:
  static constexpr auto offsets_size=offsetof(AVDOVIMetadata,color_offset)+sizeof(std::size_t);

  static const uint8_t* rpu_section(const AVFrameSideData& rpu, std::size_t field,
                                   std::size_t length) noexcept {
    std::size_t offset=0;
    std::memcpy(&offset,rpu.data+field,sizeof(offset));
    if(offset<offsets_size || offset>rpu.size || rpu.size-offset<length)return nullptr;
    return rpu.data+offset;
  }

  static Config in_band_config(const AVFrame& frame,const AVFrameSideData& rpu) noexcept {
    using Header=AVDOVIRpuDataHeader;
    const auto* header=rpu_section(rpu,offsetof(AVDOVIMetadata,header_offset),
                                  offsetof(Header,disable_residual_flag)+sizeof(uint8_t));
    // Use the HEVC RPU profile rules used by FFmpeg when a muxer omits the configuration record.
    if(header && header[offsetof(Header,vdr_rpu_profile)]==0 &&
       header[offsetof(Header,bl_video_full_range_flag)]==1)return {5,0,true};
    if(header && header[offsetof(Header,vdr_rpu_profile)]==1) {
      const bool enhancement=header[offsetof(Header,el_spatial_resampling_filter_flag)] &&
                             !header[offsetof(Header,disable_residual_flag)];
      if(enhancement)return {header[offsetof(Header,vdr_bit_depth)]==12?7u:4u,0,true};
      // Without the container's compatibility ID, identify HDR10 by its base-layer signal.
      const bool hdr10=frame.color_trc==AVCOL_TRC_SMPTE2084 &&
                       frame.color_primaries==AVCOL_PRI_BT2020 &&
                       frame.colorspace==AVCOL_SPC_BT2020_NCL && frame.color_range==AVCOL_RANGE_MPEG;
      return {8,hdr10?1u:0u,true};
    }
    return {0,0,true};
  }

  static bool has_base_rpu(const AVFrameSideData* rpu) noexcept {
    // These prefixes are shared by FFmpeg 5–9 and cover what libplacebo reads
    // from the base metadata. Later FFmpeg versions append extension blocks.
    return rpu && rpu->data && rpu->size>=offsets_size &&
      rpu_section(*rpu,offsetof(AVDOVIMetadata,header_offset),
                  offsetof(AVDOVIRpuDataHeader,disable_residual_flag)+sizeof(uint8_t)) &&
      rpu_section(*rpu,offsetof(AVDOVIMetadata,mapping_offset),
                  offsetof(AVDOVIDataMapping,nlq)+sizeof(AVDOVIDataMapping::nlq)) &&
      rpu_section(*rpu,offsetof(AVDOVIMetadata,color_offset),
                  offsetof(AVDOVIColorMetadata,source_max_pq)+sizeof(uint16_t));
  }
  static constexpr const char* key="frzz_reaper_media_import.dovi_profile";
  decltype(&av_frame_get_side_data) side_data_{};
  decltype(&av_dict_get) get_{};
  decltype(&av_dict_set_int) set_{};
};

}
}

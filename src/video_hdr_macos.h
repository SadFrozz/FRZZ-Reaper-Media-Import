#pragma once

#include "platform.h"

struct AVCodecContext;
struct AVDOVIDecoderConfigurationRecord;
struct AVFormatContext;
struct AVFrame;

namespace frzz::FRZZ_MACOS_HDR {
bool initialize_macos_hdr(ModuleHandle util) noexcept;
void prepare_macos_hdr(const AVFormatContext* format) noexcept;
void finish_macos_hdr_preparation() noexcept;
void remember_macos_hdr_frame(const AVCodecContext* codec,AVFrame* frame,
                             const AVDOVIDecoderConfigurationRecord* fallback=nullptr) noexcept;
bool render_macos_hdr(AVFrame* dst,const AVFrame* src,int flags) noexcept;
bool mark_output(AVFrame* output) noexcept;
}

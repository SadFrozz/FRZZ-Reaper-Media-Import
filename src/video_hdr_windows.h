#pragma once
#include "platform.h"
struct AVCodecContext;
struct AVCodecParameters;
struct AVDOVIDecoderConfigurationRecord;
struct AVFrame;
struct AVBufferRef;
struct AVFormatContext;
namespace frzz::FRZZ_WINDOWS_HDR {
bool initialize_windows_hdr(ModuleHandle util) noexcept;
void prepare_windows_hdr_stream(const AVFormatContext* input) noexcept;
void remember_windows_hdr_parameters(const AVCodecParameters* codec,
                                     const AVDOVIDecoderConfigurationRecord* fallback) noexcept;
void prepare_windows_hdr_device(const char* device) noexcept;
void calibrate_windows_hdr_device(const AVBufferRef* device) noexcept;
void prepare_windows_hdr_decoder(const AVCodecContext& codec,
                                const AVDOVIDecoderConfigurationRecord* fallback=nullptr) noexcept;
void stop_windows_hdr_preparation() noexcept;
void remember_windows_hdr_frame(const AVCodecContext* codec,AVFrame* frame,
                                const AVDOVIDecoderConfigurationRecord* fallback=nullptr) noexcept;
bool render_windows_hdr(AVFrame* output,const AVFrame* source,int flags) noexcept;
}

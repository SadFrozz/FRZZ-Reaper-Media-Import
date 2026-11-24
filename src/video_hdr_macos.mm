#ifdef __x86_64__
#define GL_SILENCE_DEPRECATION
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/CGLIOSurface.h>
#import <OpenGL/gl3.h>
#include <mutex>
#include "video_hdr_macos.h"
#include "video_hdr_frame.h"
#include "video_hdr_crop.h"
#include "video_hdr_placebo.h"
#include "api.h"
#include "support.h"

#include <cstring>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/buffer.h>
#include <libavutil/dovi_meta.h>
}

namespace frzz::FRZZ_MACOS_HDR {
namespace {
struct FrameFunctions {
    decltype(&av_frame_alloc) allocate{};
    decltype(&av_frame_free) release{};
    decltype(&av_frame_unref) unref{};
    decltype(&av_frame_move_ref) move{};
    decltype(&av_frame_new_side_data) side_data{};
    decltype(&av_buffer_create) buffer{};
};
FrameFunctions ffmpeg;

HdrFrameDovi frame_dovi;
std::mutex renderer_mutex;
std::mutex preparation_mutex;
struct PreparationThread {
    std::thread worker;
    ~PreparationThread() { if(worker.joinable())worker.join(); }
};
PreparationThread preparation;
bool allow_preparation{};
bool preparation_started{};
bool preparation_stopping{};

static CGLContextObj new_context()
{
    const CGLPixelFormatAttribute attributes[]={kCGLPFAAccelerated,kCGLPFAOpenGLProfile,
          static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core),static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format=nullptr;
    GLint count=0;
    if(CGLChoosePixelFormat(attributes,&format,&count)!=kCGLNoError || !format)
        return nullptr;
    CGLContextObj context=nullptr;
    const auto result=CGLCreateContext(format,nullptr,&context);
    CGLDestroyPixelFormat(format);
    return result==kCGLNoError?context:nullptr;
}

static void release_output(void* opaque,uint8_t*) {
    auto buffer=static_cast<CVPixelBufferRef>(opaque);
    CVPixelBufferUnlockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly);
    CFRelease(buffer);
}
static CVPixelBufferRef get_sdr_buffer(int w,int h) {
    static CVPixelBufferPoolRef pool;
    static int width{},height{};
    if(!pool || w!=width || h!=height) {
        if(pool)CFRelease(pool);
        pool=nullptr;
        width=w;
        height=h;
        CFNumberRef wn=CFNumberCreate(nullptr,kCFNumberIntType,&w);
        CFNumberRef hn=CFNumberCreate(nullptr,kCFNumberIntType,&h);
        uint32_t fmt=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef fn=CFNumberCreate(nullptr,kCFNumberSInt32Type,&fmt);
        CFDictionaryRef surface=CFDictionaryCreate(nullptr,nullptr,nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        const void* keys[]={kCVPixelBufferWidthKey,kCVPixelBufferHeightKey,kCVPixelBufferPixelFormatTypeKey,kCVPixelBufferIOSurfacePropertiesKey};
        const void* values[]={wn,hn,fn,surface};
        if(wn && hn && fn && surface) {
            auto attrs=CFDictionaryCreate(nullptr,keys,values,4,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            if(attrs) {
                CVPixelBufferPoolCreate(nullptr,nullptr,attrs,&pool);
                CFRelease(attrs);
            }
        }
        if(surface)CFRelease(surface);
        if(fn)CFRelease(fn);
        if(hn)CFRelease(hn);
        if(wn)CFRelease(wn);
    }
    CVPixelBufferRef result=nullptr;
    if(pool)CVPixelBufferPoolCreatePixelBuffer(nullptr,pool,&result);
    return result;
}
bool is_p010(const AVFrame* frame) {
    return frame && frame->format == AV_PIX_FMT_VIDEOTOOLBOX && frame->data[3] &&
        frame->hw_frames_ctx && frame->hw_frames_ctx->data &&
        reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data)->sw_format == AV_PIX_FMT_P010LE;
}

enum class RenderPass {playback,prepare_static,prepare_dynamic};

bool render(AVFrame* dst,const AVFrame* src,CVPixelBufferRef pb,bool apply_rpu,
            RenderPass pass=RenderPass::playback,unsigned prepare_dovi_profile=0) {
    HdrCrop crop{};
    if(!hdr_crop(src,&crop))return false;
    auto surface=CVPixelBufferGetIOSurface(pb);
    if(!surface || CVPixelBufferGetPlaneCount(pb)!=2)return false;

    std::lock_guard<std::mutex> lock(renderer_mutex);
    static bool live_render_started=false;
    const bool preparing=pass!=RenderPass::playback;
    if(preparing && live_render_started)return false;
    if(!preparing)live_render_started=true;
    static CGLContextObj context=new_context();
    if(!context)return false;
    const auto previous=CGLGetCurrentContext();
    if(CGLSetCurrentContext(context)!=kCGLNoError)return false;
    unsigned planes[2]{};
    const int formats[]={GL_R16,GL_RG16};
    const int components[]={GL_RED,GL_RG};
    int widths[2]{},heights[2]{};
    glGenTextures(2,planes);
    bool ok=true;
    for(unsigned p=0;p<2;++p) {
        widths[p]=static_cast<int>(CVPixelBufferGetWidthOfPlane(pb,p));
        heights[p]=static_cast<int>(CVPixelBufferGetHeightOfPlane(pb,p));
        if(widths[p] < ((crop.left+crop.width)>>p) || heights[p] < ((crop.top+crop.height)>>p)) {ok=false;break;}
        glBindTexture(GL_TEXTURE_RECTANGLE,planes[p]);
        if(CGLTexImageIOSurface2D(context,GL_TEXTURE_RECTANGLE,formats[p],widths[p],heights[p],components[p],
                                 GL_UNSIGNED_SHORT,surface,p)!=kCGLNoError) {ok=false;break;}
    }
    auto sdr=ok?get_sdr_buffer(crop.width,crop.height):nullptr;
    auto output=sdr?ffmpeg.allocate():nullptr;
    unsigned sdr_planes[2]{};
    glGenTextures(2,sdr_planes);
    ok = ok && sdr && output && CVPixelBufferGetIOSurface(sdr) && CVPixelBufferGetPlaneCount(sdr)==2;
    if(ok) {
        output->width=crop.width;
        output->height=crop.height;
    }
    if(ok)for(unsigned p=0;p<2;++p) {
        glBindTexture(GL_TEXTURE_RECTANGLE,sdr_planes[p]);
        if(CGLTexImageIOSurface2D(context,GL_TEXTURE_RECTANGLE,p?GL_RG8:GL_R8,crop.width>>p,crop.height>>p,components[p],
                                 GL_UNSIGNED_BYTE,CVPixelBufferGetIOSurface(sdr),p)!=kCGLNoError) {ok=false;break;}
    }
    if(ok)ok=FRZZ_HDR_API(frzz_hdr_render)(output,src,planes,sdr_planes,widths,heights,apply_rpu,
                                           pass==RenderPass::prepare_dynamic,
                                           prepare_dovi_profile);
    if(ok)glFinish();
    if(ok && pass!=RenderPass::prepare_dynamic) {
        if(CVPixelBufferLockBaseAddress(sdr,kCVPixelBufferLock_ReadOnly)!=kCVReturnSuccess)ok=false;
        else {
            for(unsigned p=0;p<2;++p) {
                output->data[p]=static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(sdr,p));
                output->linesize[p]=static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(sdr,p));
            }
            output->buf[0]=ffmpeg.buffer(output->data[0],CVPixelBufferGetDataSize(sdr),release_output,sdr,AV_BUFFER_FLAG_READONLY);
            if(!output->buf[0]) {
                CVPixelBufferUnlockBaseAddress(sdr,kCVPixelBufferLock_ReadOnly);
                ok=false;
            } else {
                sdr=nullptr;
                ok=mark_output(output);
                if(ok) {
                    ffmpeg.unref(dst);
                    ffmpeg.move(dst,output);
                }
            }
        }
    }
    if(sdr)CFRelease(sdr);
    ffmpeg.release(&output);
    glDeleteTextures(2,sdr_planes);
    glDeleteTextures(2,planes);
    CGLSetCurrentContext(previous);
    return ok;
}
CVPixelBufferRef make_preparation_surface(int size) {
    const CFDictionaryRef properties=CFDictionaryCreate(nullptr,nullptr,nullptr,0,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if(!properties)return nullptr;
    const void* key=kCVPixelBufferIOSurfacePropertiesKey;
    const void* value=properties;
    const auto attributes=CFDictionaryCreate(nullptr,&key,&value,1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef result=nullptr;
    if(attributes) {
        CVPixelBufferCreate(nullptr,size,size,kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,attributes,&result);
        CFRelease(attributes);
    }
    CFRelease(properties);
    return result;
}

void copy_static_hdr_metadata(AVFrame* frame,HdrStreamSideData metadata) {
    for(int i=0;i<metadata.count;++i) {
        const auto& side=metadata.entries[i];
        AVFrameSideDataType kind;
        switch(side.type) {
            case AV_PKT_DATA_MASTERING_DISPLAY_METADATA: kind=AV_FRAME_DATA_MASTERING_DISPLAY_METADATA;break;
            case AV_PKT_DATA_CONTENT_LIGHT_LEVEL: kind=AV_FRAME_DATA_CONTENT_LIGHT_LEVEL;break;
            default: continue;
        }
        if(side.data && side.size) {
            if(auto* copy=ffmpeg.side_data(frame,kind,side.size))
                std::memcpy(copy->data,side.data,side.size);
        }
    }
}

}

bool initialize_macos_hdr(ModuleHandle util) noexcept {
#define LOAD(member, symbol) ffmpeg.member=reinterpret_cast<decltype(ffmpeg.member)>(module_symbol(util,#symbol)); if(!ffmpeg.member)return false
    LOAD(allocate,av_frame_alloc);
    LOAD(release,av_frame_free);
    LOAD(unref,av_frame_unref);
    LOAD(move,av_frame_move_ref);
    LOAD(side_data,av_frame_new_side_data);
    LOAD(buffer,av_buffer_create);
#undef LOAD
    if(!frame_dovi.bind(util) || !FRZZ_HDR_API(frzz_hdr_bind_av)(util))return false;
    // Timing hooks are installed on the main thread. Snapshot the preference
    // before a demux callback can request preparation on a media thread.
    char priorities[512]{};
    if(get_config_var_string && get_config_var_string("video_decprio",priorities,sizeof(priorities))) {
        std::string_view list(priorities);
        const auto first=list.find_first_not_of(" ;\t\r\n");
        if(first!=list.npos) {
            list.remove_prefix(first);
            const auto decoder=list.substr(0,list.find_first_of(": ;\t\r\n"));
            allow_preparation=decoder=="ffmpeg_hw" || decoder=="ffmpeg_hw_mta" || decoder=="ffmpeg_mta_hw";
        }
    }
    return true;
}

void prepare_macos_hdr(const AVFormatContext* format) noexcept {
    if(!allow_preparation || !format)return;
    try {
        std::lock_guard<std::mutex> lock(preparation_mutex);
        if(preparation_started || preparation_stopping)return;
        const AVStream* video_stream=nullptr;
        for(unsigned i=0;i<format->nb_streams;++i) {
            const auto* stream=format->streams[i];
            const auto* parameters=stream->codecpar;
            if(parameters->codec_type!=AVMEDIA_TYPE_VIDEO || stream->disposition&AV_DISPOSITION_ATTACHED_PIC)continue;
            if(video_stream)return;
            video_stream=stream;
        }
        const auto* video=video_stream?video_stream->codecpar:nullptr;
        if(!video || video->codec_id!=AV_CODEC_ID_HEVC)return;
        const auto metadata=hdr_stream_side_data(*video_stream);
        const auto config=hdr_dovi_config(metadata.entries,metadata.count);
        const unsigned dovi_profile=config.dv_profile==5 || config.dv_profile==7?config.dv_profile:0;
        bool dynamic_metadata=dovi_profile!=0;
        for(int i=0;i<metadata.count;++i) {
            const auto& side=metadata.entries[i];
            if(side.type==AV_PKT_DATA_DYNAMIC_HDR10_PLUS)dynamic_metadata=true;
        }
        if(!dynamic_metadata && video->color_trc!=AVCOL_TRC_SMPTE2084)return;

        constexpr int sample_size=128;
        auto* sample=ffmpeg.allocate();
        if(!sample)return;
        sample->format=AV_PIX_FMT_P010LE;
        sample->width=sample->height=sample_size;
        sample->sample_aspect_ratio=video->sample_aspect_ratio;
        sample->color_range=video->color_range;
        sample->color_primaries=video->color_primaries;
        sample->color_trc=video->color_trc;
        sample->colorspace=video->color_space;
        sample->chroma_location=video->chroma_location;
        if(!dynamic_metadata || dovi_profile)copy_static_hdr_metadata(sample,metadata);
        try {
            preparation.worker=std::thread([sample,dynamic_metadata,dovi_profile]() mutable {
                auto* surface=make_preparation_surface(sample_size);
                auto* output=ffmpeg.allocate();
                bool ready=false;
                try {
                    const auto pass=dynamic_metadata?RenderPass::prepare_dynamic:RenderPass::prepare_static;
                    if(surface && output)ready=render(output,sample,surface,false,pass,dovi_profile);
                } catch(...) {}
                if(surface)CFRelease(surface);
                ffmpeg.release(&output);
                ffmpeg.release(&sample);
                if(ready)log(dynamic_metadata?"VideoToolbox HDR output shaders prepared":"VideoToolbox HDR10 shaders prepared");
            });
            preparation_started=true;
        } catch(...) {
            ffmpeg.release(&sample);
        }
    } catch(...) {}
}

void finish_macos_hdr_preparation() noexcept {
    std::thread running;
    {
        std::lock_guard<std::mutex> lock(preparation_mutex);
        preparation_stopping=true;
        running=std::move(preparation.worker);
    }
    if(running.joinable())running.join();
}

void remember_macos_hdr_frame(const AVCodecContext* codec,AVFrame* frame,
                              const AVDOVIDecoderConfigurationRecord* fallback) noexcept {
    if(is_p010(frame))frame_dovi.remember(*codec,*frame,fallback?*fallback:AVDOVIDecoderConfigurationRecord{});
}

bool render_macos_hdr(AVFrame* dst,const AVFrame* src,int flags) noexcept {
    if(!dst || flags || !is_p010(src))return false;
    const auto [profile,compatibility,has_rpu]=frame_dovi.get(*src);
#ifdef FRZZ_HDR_LEGACY_METADATA
    if(!profile && has_rpu)return false;
#endif
    const bool apply_rpu=(profile==5 || profile==7) && has_rpu;
    const bool hdr10=src->color_trc==AVCOL_TRC_SMPTE2084 &&
        (profile==0 || profile==7 || (profile==8 && compatibility==1));
    if(apply_rpu || hdr10) {
        try {
            if(render(dst,src,reinterpret_cast<CVPixelBufferRef>(src->data[3]),apply_rpu))return true;
        } catch(...) {}
    }
    return false;
}
}
#endif

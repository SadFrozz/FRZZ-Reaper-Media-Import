#include "video_hdr_windows.h"
#include "video_hdr_frame.h"
#include "video_hdr_symbols.h"
#include "video_hdr_shader_cache.h"
#include "api.h"
#include "support.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>
#include <d3d11_3.h>
#include <dxgi.h>
#include <wrl/client.h>
#ifdef FRZZ_WINDOWS_QSV
#include <d3d11_4.h>
#include <vpl/mfxstructures.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
bool FRZZ_HDR_API(frzz_d3d11_bind)(void* util,void* (*lookup)(void*,const char*));
bool FRZZ_HDR_API(frzz_d3d11_has_hdr10plus)(const AVFrame* source);
bool FRZZ_HDR_API(frzz_d3d11_decoder_is_initialized)();
bool FRZZ_HDR_API(frzz_d3d11_prepare_decoder)(const AVD3D11VADeviceContext* device,
                                            const AVFrame* sample,unsigned dovi_profile);
int FRZZ_HDR_API(frzz_d3d11_render)(AVFrame* dst,const AVFrame* source,bool apply_rpu,
                     const AVFrame* mapped,AVD3D11VADeviceContext* mapped_device);
void FRZZ_HDR_API(frzz_hdr_prepare_dxva2)(const char* selector) noexcept;
void FRZZ_HDR_API(frzz_hdr_calibrate_dxva2)(const AVHWDeviceContext* device) noexcept;
void FRZZ_HDR_API(frzz_hdr_clear_dxva2_preparation)() noexcept;
}

extern "C" bool FRZZ_HDR_API(frzz_d3d11_prefers_linear_output)(ID3D11Device* device)
{
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC description{};
  return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
    SUCCEEDED(adapter->GetDesc(&description)) && description.VendorId==0x1002 && description.DeviceId==0x164c;
}

extern "C" bool FRZZ_HDR_API(frzz_d3d11_create_readable)(ID3D11Device* device,UINT width,UINT height,ID3D11Texture2D** texture)
{
  D3D11_FEATURE_DATA_D3D11_OPTIONS2 caps{};
  if(FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,&caps,sizeof(caps))) ||
     !caps.MapOnDefaultTextures || !caps.UnifiedMemoryArchitecture)return false;
  // On this AMD APU, GPU work followed by a staging copy is faster than
  // writing to a CPU-readable render target, even with unified memory.
  if(FRZZ_HDR_API(frzz_d3d11_prefers_linear_output)(device))return false;
  Microsoft::WRL::ComPtr<ID3D11Device3> extended;
  if(FAILED(device->QueryInterface(IID_PPV_ARGS(&extended))))return false;
  D3D11_TEXTURE2D_DESC1 info{};
  info.Width=width;
  info.Height=height;
  info.MipLevels=info.ArraySize=info.SampleDesc.Count=1;
  info.Format=DXGI_FORMAT_R8_UNORM;
  info.Usage=D3D11_USAGE_DEFAULT;
  info.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
  info.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D1> created;
  if(FAILED(extended->CreateTexture2D1(&info,nullptr,&created)))return false;
  *texture=created.Detach();
  return true;
}

extern "C" bool FRZZ_HDR_API(frzz_d3d11_read_output)(const AVD3D11VADeviceContext* hw,ID3D11Texture2D* texture,AVFrame* frame)
{
  Microsoft::WRL::ComPtr<ID3D11Device3> extended;
  if(FAILED(hw->device->QueryInterface(IID_PPV_ARGS(&extended))) ||
     FAILED(hw->device_context->Map(texture,0,D3D11_MAP_READ,0,nullptr)))return false;
  const auto width=static_cast<UINT>(frame->width),height=static_cast<UINT>(frame->height);
  const D3D11_BOX luma{0,0,0,width,height,1};
  const D3D11_BOX chroma{0,height,0,width,height+height/2,1};
  extended->ReadFromSubresource(frame->data[0],frame->linesize[0],0,texture,0,&luma);
  extended->ReadFromSubresource(frame->data[1],frame->linesize[1],0,texture,0,&chroma);
  hw->device_context->Unmap(texture,0);
  return true;
}

namespace frzz::FRZZ_WINDOWS_HDR {
namespace {
HdrFrameDovi frame_dovi;
enum class Dxva2Preparation {none,device,calibration};
thread_local Dxva2Preparation decoder_preparation{};
bool prepare_from_stream{};
std::atomic<bool> stream_preparation_requested{};
std::mutex decoder_preparation_mutex;
HANDLE decoder_preparation_thread{};
bool decoder_preparation_stopping{};
decltype(&av_buffer_ref) retain_device{};
decltype(&av_buffer_unref) release_device{};

struct DecoderPreparation {
  AVBufferRef* owner{};
  AVFrame sample{};
  unsigned dovi_profile{};
  ~DecoderPreparation(){if(owner)release_device(&owner);}
};

DWORD WINAPI prepare_decoder(void* data) {
  std::unique_ptr<DecoderPreparation> preparation(static_cast<DecoderPreparation*>(data));
  const auto* context=reinterpret_cast<const AVHWDeviceContext*>(preparation->owner->data);
  const auto started=std::chrono::steady_clock::now();
  const bool prepared=FRZZ_HDR_API(frzz_d3d11_prepare_decoder)(
    static_cast<const AVD3D11VADeviceContext*>(context->hwctx),&preparation->sample,preparation->dovi_profile);
  if(prepared) {
    const std::chrono::duration<double,std::milli> elapsed=std::chrono::steady_clock::now()-started;
    frzz::log("HDR D3D11 decode-device preparation %.3f ms",elapsed.count());
  }
  return 0;
}

#ifdef FRZZ_WINDOWS_QSV
decltype(&av_frame_alloc) qsv_frame_alloc{};
decltype(&av_frame_free) qsv_frame_free{};
decltype(&av_hwframe_map) qsv_map{};

int render_qsv(AVFrame* output,const AVFrame* source,bool apply_rpu) {
  if(!qsv_frame_alloc || !qsv_frame_free || !qsv_map || !source->buf[0])return 0;
  auto* surface=reinterpret_cast<mfxFrameSurface1*>(source->data[3]);
  if(surface->Version.Major!=1 || !surface->FrameInterface || !surface->Data.MemId)return 0;
  const auto* api=surface->FrameInterface;
  // FFmpeg's QSV decoder can return a surface before decode completes.
  // Require oneVPL's public readiness callback before sampling the texture.
  if(api->Version.Major!=1 || !api->Synchronize || api->Synchronize(surface,0)!=MFX_ERR_NONE)return 0;
  struct Mapping {
    AVFrame* frame;
    ~Mapping() {qsv_frame_free(&frame);}
  } mapping{qsv_frame_alloc()};
  if(!mapping.frame)return 0;
  mapping.frame->format=AV_PIX_FMT_D3D11;
  if(qsv_map(mapping.frame,source,AV_HWFRAME_MAP_READ|AV_HWFRAME_MAP_DIRECT)<0 ||
     !mapping.frame->data[0])return 0;

  auto* texture=reinterpret_cast<ID3D11Texture2D*>(mapping.frame->data[0]);
#if LIBAVCODEC_VERSION_MAJOR == 59
  // FFmpeg 5.0 exposes the single-texture QSV index as MFX_INFINITE.
  if(reinterpret_cast<uintptr_t>(mapping.frame->data[1])==MFX_INFINITE) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if(description.ArraySize==1)mapping.frame->data[1]=nullptr;
  }
#endif
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  Microsoft::WRL::ComPtr<ID3D11Multithread> threading;
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC description{};
  texture->GetDevice(&device);
  if(!device || FAILED(device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
     FAILED(adapter->GetDesc(&description)) || description.VendorId!=0x8086)return 0;
  device->GetImmediateContext(&context);
  if(!context || FAILED(context.As(&threading)) || !threading->GetMultithreadProtected())return 0;
  AVD3D11VADeviceContext hardware{};
  hardware.device=device.Get();
  hardware.device_context=context.Get();
  hardware.lock_ctx=threading.Get();
  hardware.lock=[](void* lock){static_cast<ID3D11Multithread*>(lock)->Enter();};
  hardware.unlock=[](void* lock){static_cast<ID3D11Multithread*>(lock)->Leave();};
  return FRZZ_HDR_API(frzz_d3d11_render)(output,source,apply_rpu,mapping.frame,&hardware);
}
#endif

Dxva2Preparation dxva2_preparation(const AVCodecParameters& codec,HdrStreamSideData data) {
  if(codec.codec_type!=AVMEDIA_TYPE_VIDEO || codec.codec_id!=AV_CODEC_ID_HEVC)return Dxva2Preparation::none;
  if(codec.color_trc==AVCOL_TRC_SMPTE2084)return Dxva2Preparation::calibration;
  const auto config=hdr_dovi_config(data.entries,data.count);
  if(config.dv_profile==5 || config.dv_profile==7)return Dxva2Preparation::calibration;
#if LIBAVCODEC_VERSION_MAJOR < 60
  constexpr int main10=FF_PROFILE_HEVC_MAIN_10;
#else
  constexpr int main10=AV_PROFILE_HEVC_MAIN_10;
#endif
  // Untagged Profile 5 can expose its color information only on the decoded frame.
  // Prepare the D3D11 device while the codec is unknown. Calibrating here could
  // keep an ordinary SDR decoder alive after its stream has closed.
  if(!config.dv_profile && codec.color_trc==AVCOL_TRC_UNSPECIFIED &&
     (codec.profile==main10 || codec.format==AV_PIX_FMT_YUV420P10LE))return Dxva2Preparation::device;
  return Dxva2Preparation::none;
}

const uint8_t* p010_surface(const AVFrame* frame) {
  if(!frame || !frame->hw_frames_ctx || !frame->hw_frames_ctx->data)return nullptr;
  const auto* frames=reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
  if(frames->sw_format!=AV_PIX_FMT_P010LE)return nullptr;
  if(frame->format==AV_PIX_FMT_D3D11)return frame->data[0];
  if(frame->format==AV_PIX_FMT_DXVA2_VLD)return frame->data[3];
#ifdef FRZZ_WINDOWS_QSV
  if(frame->format==AV_PIX_FMT_QSV && frames->format==AV_PIX_FMT_QSV && frames->device_ctx &&
     frames->device_ctx->type==AV_HWDEVICE_TYPE_QSV)return frame->data[3];
#endif
  return nullptr;
}
}

bool initialize_windows_hdr(ModuleHandle util) noexcept {
  retain_device=reinterpret_cast<decltype(retain_device)>(module_symbol(util,"av_buffer_ref"));
  release_device=reinterpret_cast<decltype(release_device)>(module_symbol(util,"av_buffer_unref"));
  if(!frame_dovi.bind(util) || !FRZZ_HDR_API(frzz_d3d11_bind)(util,[](void* module,const char* symbol) {
    return module_symbol(static_cast<ModuleHandle>(module),symbol);
  }))return false;
#ifdef FRZZ_WINDOWS_QSV
  qsv_frame_alloc=reinterpret_cast<decltype(qsv_frame_alloc)>(module_symbol(util,"av_frame_alloc"));
  qsv_frame_free=reinterpret_cast<decltype(qsv_frame_free)>(module_symbol(util,"av_frame_free"));
  qsv_map=reinterpret_cast<decltype(qsv_map)>(module_symbol(util,"av_hwframe_map"));
#endif

  // The installer runs on the main thread, before FFmpeg's demux callbacks.
  char priorities[512]{};
  if(get_config_var_string && get_config_var_string("video_decprio",priorities,sizeof(priorities))) {
    std::string_view list(priorities);
    const auto first=list.find_first_not_of(" ;\t\r\n");
    if(first!=list.npos) {
      list.remove_prefix(first);
      const auto decoder=list.substr(0,list.find_first_of(": ;\t\r\n"));
      prepare_from_stream=decoder=="ffmpeg_hw" || decoder=="ffmpeg_hw_mta" || decoder=="ffmpeg_mta_hw";
    }
  }
  return true;
}

void prepare_windows_hdr_stream(const AVFormatContext* input) noexcept {
  if(!prepare_from_stream || !input || (stream_preparation_requested.load() && frzz_hdr_warmup_dovi_profile()))return;
  const AVStream* video=nullptr;
  for(unsigned i=0;i<input->nb_streams;++i) {
    const auto* stream=input->streams[i];
    if(!stream || !stream->codecpar || stream->codecpar->codec_type!=AVMEDIA_TYPE_VIDEO ||
       (stream->disposition&AV_DISPOSITION_ATTACHED_PIC))continue;
    if(video)return;
    video=stream;
  }
  if(!video)return;
  const auto data=hdr_stream_side_data(*video);
  if(dxva2_preparation(*video->codecpar,data)==Dxva2Preparation::none)return;
  // Dolby Vision configuration may only be available after stream analysis.
  frzz_hdr_hint_dovi_shader(hdr_dovi_config(data.entries,data.count).dv_profile);
  if(!stream_preparation_requested.exchange(true))
    FRZZ_HDR_API(frzz_hdr_prepare_dxva2)(nullptr);
}

void remember_windows_hdr_parameters(const AVCodecParameters* codec,
                                      const AVDOVIDecoderConfigurationRecord* fallback) noexcept {
  if(!codec || codec->codec_type!=AVMEDIA_TYPE_VIDEO)return;
  HdrStreamSideData metadata{};
#if LIBAVCODEC_VERSION_MAJOR >= 61
  metadata={codec->coded_side_data,codec->nb_coded_side_data};
#endif
  decoder_preparation=dxva2_preparation(*codec,metadata);
  if(codec->codec_id==AV_CODEC_ID_HEVC && fallback && (fallback->dv_profile==5 || fallback->dv_profile==7))
    decoder_preparation=Dxva2Preparation::calibration;
}

void prepare_windows_hdr_device(const char* device) noexcept {
  if(decoder_preparation!=Dxva2Preparation::none)FRZZ_HDR_API(frzz_hdr_prepare_dxva2)(device);
}

void calibrate_windows_hdr_device(const AVBufferRef* device) noexcept {
  if(decoder_preparation==Dxva2Preparation::calibration && device)
    FRZZ_HDR_API(frzz_hdr_calibrate_dxva2)(reinterpret_cast<const AVHWDeviceContext*>(device->data));
}

void prepare_windows_hdr_decoder(const AVCodecContext& codec,
                                 const AVDOVIDecoderConfigurationRecord* fallback) noexcept {
  if(!codec.hw_device_ctx || !codec.hw_device_ctx->data || !retain_device || !release_device ||
     codec.width<=0 || codec.height<=0 || (codec.width&1) || (codec.height&1) ||
     FRZZ_HDR_API(frzz_d3d11_decoder_is_initialized)())return;
  const auto* context=reinterpret_cast<const AVHWDeviceContext*>(codec.hw_device_ctx->data);
  if(context->type!=AV_HWDEVICE_TYPE_D3D11VA || !context->hwctx)return;
  auto config=hdr_dovi_config(codec.coded_side_data,codec.nb_coded_side_data);
  if(!config.dv_profile && fallback)config=*fallback;
  const unsigned dovi_profile=config.dv_profile==5 || config.dv_profile==7?config.dv_profile:0;
  if(!dovi_profile && codec.color_trc!=AVCOL_TRC_SMPTE2084)return;
  try {
    std::lock_guard<std::mutex> lock(decoder_preparation_mutex);
    if(decoder_preparation_stopping || FRZZ_HDR_API(frzz_d3d11_decoder_is_initialized)())return;
    if(decoder_preparation_thread) {
      if(WaitForSingleObject(decoder_preparation_thread,0)!=WAIT_OBJECT_0)return;
      CloseHandle(decoder_preparation_thread);
      decoder_preparation_thread=nullptr;
      if(FRZZ_HDR_API(frzz_d3d11_decoder_is_initialized)())return;
    }
    auto preparation=std::make_unique<DecoderPreparation>();
    preparation->owner=retain_device(codec.hw_device_ctx);
    if(!preparation->owner)return;
    preparation->dovi_profile=dovi_profile;
    auto& sample=preparation->sample;
    sample.width=codec.width;
    sample.height=codec.height;
    sample.format=AV_PIX_FMT_P010LE;
    sample.colorspace=codec.colorspace;
    sample.color_trc=codec.color_trc;
    sample.color_range=codec.color_range;
    sample.color_primaries=codec.color_primaries;
    sample.chroma_location=codec.chroma_sample_location;
    auto* data=preparation.release();
    decoder_preparation_thread=CreateThread(nullptr,0,prepare_decoder,data,0,nullptr);
    if(!decoder_preparation_thread)delete data;
  } catch(...) {}
}

void stop_windows_hdr_preparation() noexcept {
  HANDLE worker{};
  {
    std::lock_guard<std::mutex> lock(decoder_preparation_mutex);
    decoder_preparation_stopping=true;
    worker=decoder_preparation_thread;
    decoder_preparation_thread=nullptr;
  }
  if(worker) {
    WaitForSingleObject(worker,INFINITE);
    CloseHandle(worker);
  }
  FRZZ_HDR_API(frzz_hdr_clear_dxva2_preparation)();
}

void remember_windows_hdr_frame(const AVCodecContext* codec,AVFrame* frame,
                                const AVDOVIDecoderConfigurationRecord* fallback) noexcept {
  if(p010_surface(frame))frame_dovi.remember(*codec,*frame,fallback?*fallback:AVDOVIDecoderConfigurationRecord{});
}

bool render_windows_hdr(AVFrame* output,const AVFrame* source,int flags) noexcept {
  if(!output || flags)return false;
  if(!p010_surface(source))return false;
  const auto [profile,compatibility,has_rpu]=frame_dovi.get(*source);
#ifdef FRZZ_HDR_LEGACY_METADATA
  if(!profile && has_rpu)return false;
#endif
  const bool apply_rpu=(profile==5 || profile==7) && has_rpu;
  const bool hdr10=source->color_trc==AVCOL_TRC_SMPTE2084 &&
    (profile==0 || profile==7 || (profile==8 && compatibility==1));
  if(apply_rpu || hdr10) {
    try {
      const auto start=std::chrono::steady_clock::now();
      const bool qsv=source->format==AV_PIX_FMT_QSV;
      const int delivery=
#ifdef FRZZ_WINDOWS_QSV
        qsv?render_qsv(output,source,apply_rpu):
#endif
        FRZZ_HDR_API(frzz_d3d11_render)(output,source,apply_rpu,nullptr,nullptr);
      if(delivery) {
        static std::atomic<bool> logged_d3d11{};
        static std::atomic<bool> logged_dxva2{};
        static std::atomic<bool> logged_qsv{};
        const bool bridge=source->format==AV_PIX_FMT_DXVA2_VLD;
        if(!(bridge?logged_dxva2:qsv?logged_qsv:logged_d3d11).exchange(true)) {
          const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
          log("HDR %s %dx%d profile=%u rpu=%d hdr10plus=%d readback=%s first frame %.3f ms",
            bridge?"DXVA2/D3D11":qsv?"QSV/D3D11":"D3D11",output->width,output->height,profile,apply_rpu,
            !apply_rpu && FRZZ_HDR_API(frzz_d3d11_has_hdr10plus)(source),
            delivery==3?"linear":delivery==2?"direct":"staging",ms);
        }
        return true;
      }
    } catch(...) {}
  }
  return false;
}
}

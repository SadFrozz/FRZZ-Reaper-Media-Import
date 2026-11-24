#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
#include "video_hdr_native_av.h"
#include "video_hdr_crop.h"
#include "video_hdr_symbols.h"
#include "video_hdr_shader_cache.h"

static ID3D11Device* device;
static ID3D11DeviceContext1* context_version1;
static ID3D11ComputeShader* shaders[HDR_SHADER_COUNT];
static ID3D11Texture2D *sampled, *target_tex, *readback, *bridge_texture;
static ID3D11Texture3D* lattice;
static ID3D11ShaderResourceView *input_planes[2], *bridge_input, *lattice_resource;
static ID3D11UnorderedAccessView *target_view, *lattice_view, *packed_view;
static ID3D11Buffer *color_buffer, *output_buffer, *packed_buffer, *packed_readback;
static ID3D11SamplerState* sampler;
static UINT input_width, input_height, target_width, target_height, packed_size;
static HdrColor cached_color;
static bool lut_ready, target_direct, direct_failed, prefer_linear;
static SRWLOCK renderer_lock = SRWLOCK_INIT;
static volatile LONG renderer_initialized;

#define RELEASE(type, object) do { if (object) { type ## _Release(object); object = NULL; } } while (0)

static AVFrame* (*frame_alloc)(void);
static void (*frame_free)(AVFrame**);
static void (*frame_unref)(AVFrame*);
static void (*frame_move)(AVFrame*,AVFrame*);
static int (*frame_buffer)(AVFrame*,int);
static AVBufferRef* (*pixel_buffer)(size_t);
int FRZZ_HDR_API(frzz_hdr_share_dxva2)(const AVFrame*,AVD3D11VADeviceContext**,ID3D11Texture2D**,const float**);
bool FRZZ_HDR_API(frzz_d3d11_prefers_linear_output)(ID3D11Device*);
bool FRZZ_HDR_API(frzz_d3d11_create_readable)(ID3D11Device*,UINT,UINT,ID3D11Texture2D**);
bool FRZZ_HDR_API(frzz_d3d11_read_output)(const AVD3D11VADeviceContext*,ID3D11Texture2D*,AVFrame*);
bool FRZZ_HDR_API(frzz_hdr_mark_output)(AVFrame*);
bool FRZZ_HDR_API(frzz_hdr_reuse_output)(AVFrame*);
bool FRZZ_HDR_API(frzz_hdr_owns_stock_p010)(const AVFrame*);

bool FRZZ_HDR_API(frzz_d3d11_bind)(void* util,void* (*lookup)(void*,const char*))
{
  if(!bind_hdr_metadata(util,lookup))return false;
#define LOAD(member,type,symbol) member=(type)lookup(util,#symbol); if(!member)return false
  LOAD(frame_alloc,AVFrame* (*)(void),av_frame_alloc);
  LOAD(frame_free,void (*)(AVFrame**),av_frame_free);
  LOAD(frame_unref,void (*)(AVFrame*),av_frame_unref);
  LOAD(frame_move,void (*)(AVFrame*,AVFrame*),av_frame_move_ref);
  LOAD(frame_buffer,int (*)(AVFrame*,int),av_frame_get_buffer);
#undef LOAD
  pixel_buffer=(AVBufferRef* (*)(size_t))lookup(util,"av_buffer_alloc");
  return true;
}
bool FRZZ_HDR_API(frzz_d3d11_has_hdr10plus)(const AVFrame* source) {
  return hdr_get_side_data(source,AV_FRAME_DATA_DYNAMIC_HDR_PLUS)!=NULL;
}


static void stop_linear_output(void)
{
  prefer_linear = false;
  RELEASE(ID3D11UnorderedAccessView, packed_view);
  RELEASE(ID3D11Buffer, packed_buffer);
  RELEASE(ID3D11Buffer, packed_readback);
  packed_size = 0;
}

static void reset_renderer(void)
{
  stop_linear_output();
  for (int p = 0; p < 2; ++p) RELEASE(ID3D11ShaderResourceView, input_planes[p]);
  for (int i = 0; i < HDR_SHADER_COUNT; ++i) RELEASE(ID3D11ComputeShader, shaders[i]);
  RELEASE(ID3D11SamplerState, sampler);
  RELEASE(ID3D11UnorderedAccessView, lattice_view);
  RELEASE(ID3D11ShaderResourceView, lattice_resource);
  RELEASE(ID3D11Texture3D, lattice);
  RELEASE(ID3D11Buffer, color_buffer);
  RELEASE(ID3D11Buffer, output_buffer);
  RELEASE(ID3D11UnorderedAccessView, target_view);
  RELEASE(ID3D11ShaderResourceView, bridge_input);
  RELEASE(ID3D11Texture2D, sampled);
  RELEASE(ID3D11Texture2D, target_tex);
  RELEASE(ID3D11Texture2D, readback);
  RELEASE(ID3D11DeviceContext1, context_version1);
  RELEASE(ID3D11Device, device);
  bridge_texture = NULL;
  lut_ready = target_direct = direct_failed = false;
}

struct OutputConstants {
  float size[2], origin[2], shift[2], chroma_size[2];
  float bridge[4][4];
};

static bool create_buffers(void)
{
  D3D11_BUFFER_DESC buffer = {.ByteWidth = sizeof(HdrColor), .Usage = D3D11_USAGE_DEFAULT,
                             .BindFlags = D3D11_BIND_CONSTANT_BUFFER};
  if (FAILED(ID3D11Device_CreateBuffer(device, &buffer, NULL, &color_buffer))) return false;
  buffer.ByteWidth = sizeof(struct OutputConstants);
  return SUCCEEDED(ID3D11Device_CreateBuffer(device, &buffer, NULL, &output_buffer));
}

static bool start_renderer(const AVD3D11VADeviceContext* hw, bool preparing)
{
  if (device == hw->device) return true;
  reset_renderer();
  device = hw->device;
  ID3D11Device_AddRef(device);
  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {0};
  if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(device, D3D11_FEATURE_D3D11_OPTIONS,
                                                &options, sizeof(options))) && options.ConstantBufferOffsetting)
    ID3D11DeviceContext_QueryInterface(hw->device_context, &IID_ID3D11DeviceContext1, (void**)&context_version1);
  for (int i = 0; i < HDR_SHADER_COUNT; ++i) {
    shaders[i] = frzz_hdr_create_compute_shader(device, (enum HdrShader)i);
    if (!shaders[i]) goto fail;
  }
  const D3D11_SAMPLER_DESC filter = {
      .Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,
      .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP, .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
      .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP, .MaxAnisotropy = 1,
      .ComparisonFunc = D3D11_COMPARISON_NEVER, .MaxLOD = D3D11_FLOAT32_MAX,
  };
  const D3D11_TEXTURE3D_DESC volume = {
      .Width = 65, .Height = 65, .Depth = 65, .MipLevels = 1,
      .Format = DXGI_FORMAT_R16G16B16A16_FLOAT, .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
  };
  if (!create_buffers() ||
      FAILED(ID3D11Device_CreateSamplerState(device, &filter, &sampler)) ||
      FAILED(ID3D11Device_CreateTexture3D(device, &volume, NULL, &lattice)) ||
      FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource*)lattice, NULL, &lattice_resource)) ||
      FAILED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource*)lattice, NULL, &lattice_view)))
    goto fail;
  prefer_linear = pixel_buffer && FRZZ_HDR_API(frzz_d3d11_prefers_linear_output)(device);
  if (!preparing) InterlockedExchange(&renderer_initialized, 1);
  return true;
fail:
  reset_renderer();
  return false;
}

static bool prepare_texture(D3D11_TEXTURE2D_DESC decoder)
{
  if (sampled && decoder.Width == input_width && decoder.Height == input_height &&
      input_planes[0] && input_planes[1]) return true;
  for (int p = 0; p < 2; ++p) RELEASE(ID3D11ShaderResourceView, input_planes[p]);
  RELEASE(ID3D11Texture2D, sampled);
  decoder.ArraySize = 1;
  decoder.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  decoder.CPUAccessFlags = 0;
  decoder.MiscFlags = 0;
  if (FAILED(ID3D11Device_CreateTexture2D(device, &decoder, NULL, &sampled))) return false;
  input_width = decoder.Width;
  input_height = decoder.Height;
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM};
  for (int i = 0; i < 2; ++i) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view = {.Format = formats[i], .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
                                             .Texture2D = {.MipLevels = 1}};
    if (FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource*)sampled, &view, &input_planes[i])))
      return false;
  }
  return true;
}

static bool prepare_staging(const AVD3D11VADeviceContext* hw)
{
  if (readback) return true;
  D3D11_TEXTURE2D_DESC desc;
  ID3D11Texture2D_GetDesc(target_tex, &desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  return SUCCEEDED(ID3D11Device_CreateTexture2D(hw->device, &desc, NULL, &readback));
}

static bool prepare_target(const AVD3D11VADeviceContext* hw, UINT width, UINT height)
{
  if (target_view && (target_direct || readback) && target_width == width && target_height == height)
    return true;
  RELEASE(ID3D11UnorderedAccessView, target_view);
  RELEASE(ID3D11Texture2D, target_tex);
  RELEASE(ID3D11Texture2D, readback);
  const D3D11_TEXTURE2D_DESC desc = {
      .Width = width, .Height = height + height / 2, .MipLevels = 1, .ArraySize = 1,
      .Format = DXGI_FORMAT_R8_UNORM, .SampleDesc = {.Count = 1}, .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
  };
  target_direct = !direct_failed &&
      FRZZ_HDR_API(frzz_d3d11_create_readable)(hw->device, desc.Width, desc.Height, &target_tex);
  if (target_direct && FAILED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource*)target_tex,
                                                                   NULL, &target_view))) {
    RELEASE(ID3D11Texture2D, target_tex);
    target_direct = false;
    direct_failed = true;
  }
  if (!target_direct &&
      (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &target_tex)) ||
       FAILED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource*)target_tex, NULL, &target_view)) ||
       !prepare_staging(hw))) return false;
  target_width = width;
  target_height = height;
  return true;
}

static bool supports_linear_width(int width)
{
  // Preserve SIMD alignment for the NV12 pitches and the start of the UV plane.
  return width % 64 == 0;
}

static bool prepare_linear_target(int width, int height)
{
  if (!prefer_linear || !supports_linear_width(width)) return false;
  const size_t bytes = (size_t)width * ((size_t)height + height / 2);
  if (bytes == packed_size && packed_view) return true;
  if (bytes > UINT_MAX || bytes / 4 > (1u << D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP)) {
    stop_linear_output();
    return false;
  }
  RELEASE(ID3D11UnorderedAccessView, packed_view);
  RELEASE(ID3D11Buffer, packed_buffer);
  RELEASE(ID3D11Buffer, packed_readback);
  D3D11_BUFFER_DESC desc = {.ByteWidth = (UINT)bytes, .Usage = D3D11_USAGE_DEFAULT,
                           .BindFlags = D3D11_BIND_UNORDERED_ACCESS};
  const D3D11_UNORDERED_ACCESS_VIEW_DESC view = {
      .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
      .Buffer = {.NumElements = (UINT)(bytes / 4)},
  };
  const bool gpu_ready = SUCCEEDED(ID3D11Device_CreateBuffer(device, &desc, NULL, &packed_buffer)) &&
      SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource*)packed_buffer, &view, &packed_view));
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (gpu_ready && SUCCEEDED(ID3D11Device_CreateBuffer(device, &desc, NULL, &packed_readback))) {
    packed_size = (UINT)bytes;
    return true;
  }
  stop_linear_output();
  return false;
}

struct ComputeState {
  ID3D11ComputeShader* shader;
  ID3D11ClassInstance* classes[D3D11_SHADER_MAX_INTERFACES];
  UINT class_count;
  ID3D11ShaderResourceView* resources[3];
  ID3D11UnorderedAccessView* output;
  ID3D11SamplerState* sampler;
  ID3D11Buffer* constants;
  UINT first_constant, constant_count;
};

static void save_compute(ID3D11DeviceContext* context, struct ComputeState* state)
{
  state->class_count = D3D11_SHADER_MAX_INTERFACES;
  ID3D11DeviceContext_CSGetShader(context, &state->shader, state->classes, &state->class_count);
  ID3D11DeviceContext_CSGetShaderResources(context, 0, 3, state->resources);
  ID3D11DeviceContext_CSGetUnorderedAccessViews(context, 0, 1, &state->output);
  ID3D11DeviceContext_CSGetSamplers(context, 0, 1, &state->sampler);
  if (context_version1) {
    ID3D11DeviceContext1_CSGetConstantBuffers1(context_version1, 0, 1, &state->constants,
                                                &state->first_constant, &state->constant_count);
  } else {
    ID3D11DeviceContext_CSGetConstantBuffers(context, 0, 1, &state->constants);
  }
}

static void restore_compute(ID3D11DeviceContext* context, struct ComputeState* state)
{
  const UINT unchanged = UINT_MAX;
  ID3D11DeviceContext_CSSetShader(context, state->shader, state->classes, state->class_count);
  ID3D11DeviceContext_CSSetShaderResources(context, 0, 3, state->resources);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 1, &state->output, &unchanged);
  ID3D11DeviceContext_CSSetSamplers(context, 0, 1, &state->sampler);
  if (context_version1) {
    ID3D11DeviceContext1_CSSetConstantBuffers1(context_version1, 0, 1, &state->constants,
                                                &state->first_constant, &state->constant_count);
  } else {
    ID3D11DeviceContext_CSSetConstantBuffers(context, 0, 1, &state->constants);
  }
  RELEASE(ID3D11ComputeShader, state->shader);
  RELEASE(ID3D11UnorderedAccessView, state->output);
  RELEASE(ID3D11SamplerState, state->sampler);
  RELEASE(ID3D11Buffer, state->constants);
  for (UINT i = 0; i < 3; ++i) RELEASE(ID3D11ShaderResourceView, state->resources[i]);
  for (UINT i = 0; i < state->class_count; ++i) RELEASE(ID3D11ClassInstance, state->classes[i]);
}

static void ensure_color_lut(ID3D11DeviceContext* context, const HdrColor* color)
{
  if (lut_ready && !memcmp(&cached_color, color, sizeof(*color))) return;
  ID3D11ShaderResourceView* empty[3] = {0};
  ID3D11UnorderedAccessView* null = NULL;
  ID3D11DeviceContext_CSSetShaderResources(context, 0, 3, empty);
  ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource*)color_buffer, 0, NULL, color, 0, 0);
  ID3D11DeviceContext_CSSetConstantBuffers(context, 0, 1, &color_buffer);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 1, &lattice_view, NULL);
  ID3D11DeviceContext_CSSetShader(context, shaders[HDR_SHADER_COLOR], NULL, 0);
  ID3D11DeviceContext_Dispatch(context, 9, 9, 65);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 1, &null, NULL);
  cached_color = *color;
  lut_ready = true;
}

static void render_nv12(ID3D11DeviceContext* context, const AVFrame* source, struct HdrCrop crop,
                        ID3D11UnorderedAccessView* output, enum HdrShader shader, const float* inverse)
{
  struct OutputConstants constants = {
      .size = {(float)crop.width, (float)crop.height}, .origin = {(float)crop.left, (float)crop.top},
      .chroma_size = {(float)(input_width / 2), (float)(input_height / 2)},
  };
  hdr_chroma_shift(source->chroma_location, constants.shift);
  if (inverse) for (int i = 0; i < 4; ++i) memcpy(constants.bridge[i], inverse + 3 * i, 3 * sizeof(float));
  ID3D11ShaderResourceView* resources[] = {inverse ? bridge_input : input_planes[0],
                                          inverse ? NULL : input_planes[1], lattice_resource};
  ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource*)output_buffer, 0, NULL, &constants, 0, 0);
  ID3D11DeviceContext_CSSetConstantBuffers(context, 0, 1, &output_buffer);
  ID3D11DeviceContext_CSSetShaderResources(context, 0, 3, resources);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 1, &output, NULL);
  ID3D11DeviceContext_CSSetSamplers(context, 0, 1, &sampler);
  ID3D11DeviceContext_CSSetShader(context, shaders[shader], NULL, 0);
  ID3D11DeviceContext_Dispatch(context, (crop.width + 15) / 16, (crop.height + 7) / 8, 1);
  ID3D11ShaderResourceView* empty[3] = {0};
  ID3D11UnorderedAccessView* null = NULL;
  ID3D11DeviceContext_CSSetShaderResources(context, 0, 3, empty);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 1, &null, NULL);
}

bool FRZZ_HDR_API(frzz_d3d11_decoder_is_initialized)(void)
{
  return InterlockedCompareExchange(&renderer_initialized, 0, 0) != 0;
}

bool FRZZ_HDR_API(frzz_d3d11_prepare_decoder)(const AVD3D11VADeviceContext* hw,
                                            const AVFrame* source, unsigned dovi_profile)
{
  (void)dovi_profile;
  if (!hw || !hw->lock || !hw->unlock || !hw->device || !hw->device_context) return false;
  // The live renderer locks in the opposite order. Avoid waiting for its lock.
  hw->lock(hw->lock_ctx);
  if (!TryAcquireSRWLockExclusive(&renderer_lock)) {
    hw->unlock(hw->lock_ctx);
    return false;
  }
  bool prepared = false;
  if (!device) {
    prepared = start_renderer(hw, true) &&
        (prepare_linear_target(source->width, source->height) || prepare_target(hw, source->width, source->height));
    if (prepared) {
      HdrColor color;
      if (hdr_describe_color(&color, source, false)) {
        struct ComputeState previous = {0};
        save_compute(hw->device_context, &previous);
        ensure_color_lut(hw->device_context, &color);
        restore_compute(hw->device_context, &previous);
      }
      InterlockedExchange(&renderer_initialized, 1);
    } else {
      reset_renderer();
    }
  }
  ReleaseSRWLockExclusive(&renderer_lock);
  hw->unlock(hw->lock_ctx);
  return prepared;
}

static void describe_sdr_output(AVFrame* output,int width,int height)
{
  output->width=width;
  output->height=height;
  output->format=AV_PIX_FMT_NV12;
  output->color_range=AVCOL_RANGE_MPEG;
  output->colorspace=AVCOL_SPC_BT709;
  output->color_trc=AVCOL_TRC_BT709;
  output->color_primaries=AVCOL_PRI_BT709;
  output->chroma_location=AVCHROMA_LOC_LEFT;
}

static bool allocate_output_pixels(AVFrame* output,bool linear)
{
  if(!linear)return frame_buffer(output,0)>=0;
  const size_t luma_size=(size_t)output->width*output->height;
  const size_t frame_size=luma_size+luma_size/2;
  output->buf[0]=pixel_buffer(frame_size+AV_INPUT_BUFFER_PADDING_SIZE);
  if(!output->buf[0])return false;
  output->data[0]=output->buf[0]->data;
  output->data[1]=output->data[0]+luma_size;
  output->linesize[0]=output->linesize[1]=output->width;
  memset(output->data[0]+frame_size,0,AV_INPUT_BUFFER_PADDING_SIZE);
  return true;
}

static bool has_linear_pixels(const AVFrame* frame)
{
  const size_t luma_size=(size_t)frame->width*frame->height;
  return frame->data[0] && frame->linesize[0]==frame->width && frame->linesize[1]==frame->width &&
    (uintptr_t)frame->data[1]-(uintptr_t)frame->data[0]==luma_size;
}

static bool finish_output(const AVD3D11VADeviceContext* hw,AVFrame* output)
{
  if(target_direct) {
    if(FRZZ_HDR_API(frzz_d3d11_read_output)(hw,target_tex,output))return true;
    target_direct=false;
    direct_failed=true;
  }
  if(!prepare_staging(hw))return false;
  ID3D11DeviceContext_CopyResource(hw->device_context,(ID3D11Resource*)readback,(ID3D11Resource*)target_tex);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if(FAILED(ID3D11DeviceContext_Map(hw->device_context,(ID3D11Resource*)readback,0,D3D11_MAP_READ,0,&mapped)))return false;
  const size_t width=(size_t)output->width;
  const bool valid=mapped.RowPitch>=width;
  if(valid)for(int p=0;p<2;++p) {
    const uint8_t* source=(const uint8_t*)mapped.pData+(p?(size_t)mapped.RowPitch*output->height:0);
    for(int row=0;row<(output->height>>p);++row)
      memcpy(output->data[p]+(size_t)output->linesize[p]*row,source+(size_t)mapped.RowPitch*row,width);
  }
  ID3D11DeviceContext_Unmap(hw->device_context,(ID3D11Resource*)readback,0);
  return valid;
}

int FRZZ_HDR_API(frzz_d3d11_render)(AVFrame* dst,const AVFrame* source,bool apply_rpu,
                      const AVFrame* mapped,AVD3D11VADeviceContext* mapped_device)
{
  const AVFrame* surface=mapped?mapped:source;
  const bool bridged=surface->format==AV_PIX_FMT_DXVA2_VLD;
  if(!bridged && (surface->format!=AV_PIX_FMT_D3D11 || !surface->data[0]))return false;
  struct HdrCrop crop;
  if(!source->hw_frames_ctx || !source->hw_frames_ctx->data || !hdr_crop(source,&crop) ||
     (dst->format!=AV_PIX_FMT_NONE && dst->format!=AV_PIX_FMT_NV12 &&
      !FRZZ_HDR_API(frzz_hdr_owns_stock_p010)(dst)))return false;
  AVHWFramesContext* frames=(AVHWFramesContext*)source->hw_frames_ctx->data;
  if(frames->sw_format!=AV_PIX_FMT_P010LE || !frames->device_ctx)return false;
  AVD3D11VADeviceContext* hw=bridged?NULL:(mapped?mapped_device:frames->device_ctx->hwctx);
  ID3D11Texture2D* texture=bridged?NULL:(ID3D11Texture2D*)surface->data[0];
  const float* inverse=NULL;
  int delivery=0;
  AcquireSRWLockExclusive(&renderer_lock);
  if(bridged && FRZZ_HDR_API(frzz_hdr_share_dxva2)(source,&hw,&texture,&inverse)<0)goto release_lock;
  if(!hw || !hw->lock || !hw->unlock || !hw->device || !hw->device_context)goto release_lock;
  D3D11_TEXTURE2D_DESC decoder;
  ID3D11Texture2D_GetDesc(texture,&decoder);
  const UINT slice=bridged?0:(UINT)(uintptr_t)surface->data[1];
  if(decoder.Format!=(bridged?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_P010) || slice>=decoder.ArraySize ||
     (UINT)(crop.left+crop.width)>decoder.Width || (UINT)(crop.top+crop.height)>decoder.Height)
    goto release_lock;
  const bool compact=pixel_buffer && !bridged && !mapped && supports_linear_width(crop.width) &&
    (device && device==hw->device?prefer_linear:FRZZ_HDR_API(frzz_d3d11_prefers_linear_output)(hw->device));
  const bool reuse=dst->width==crop.width && dst->height==crop.height &&
    (!compact || has_linear_pixels(dst)) &&
    FRZZ_HDR_API(frzz_hdr_reuse_output)(dst);
  AVFrame* output=frame_alloc();
  if(!output)goto release_lock;
  describe_sdr_output(output,crop.width,crop.height);
  output->pts=source->pts;
#if LIBAVUTIL_VERSION_MAJOR < 58
  output->pkt_duration=source->pkt_duration;
#else
  output->duration=source->duration;
#endif
  output->best_effort_timestamp=source->best_effort_timestamp;
  output->sample_aspect_ratio=source->sample_aspect_ratio;
  if(reuse) {
    for(int i=0;i<2;++i) {
      output->data[i]=dst->data[i];
      output->linesize[i]=dst->linesize[i];
    }
  } else if(!allocate_output_pixels(output,compact) || !FRZZ_HDR_API(frzz_hdr_mark_output)(output))goto release_output;

  hw->lock(hw->lock_ctx);
  if(!start_renderer(hw,false))goto unlock;
  const bool linear=compact && prepare_linear_target(output->width,output->height);
  if(!linear && !prepare_target(hw,(UINT)output->width,(UINT)output->height))goto unlock;
  if(bridged) {
    if(!bridge_input || bridge_texture!=texture) {
      RELEASE(ID3D11ShaderResourceView, bridge_input);
      if (FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource*)texture, NULL, &bridge_input)))
        goto unlock;
      if(!bridge_input)goto unlock;
      bridge_texture=texture;
    }
  } else {
    if(!prepare_texture(decoder))goto unlock;
    ID3D11DeviceContext_CopySubresourceRegion(hw->device_context,(ID3D11Resource*)sampled,0,0,0,0,
      (ID3D11Resource*)texture,slice*decoder.MipLevels,NULL);
  }

  HdrColor color;
  if (!hdr_describe_color(&color, source, apply_rpu)) goto unlock;
  struct ComputeState previous = {0};
  save_compute(hw->device_context, &previous);
  ensure_color_lut(hw->device_context, &color);
  if (linear) {
    render_nv12(hw->device_context, source, crop, packed_view, HDR_SHADER_PACKED, NULL);
    ID3D11DeviceContext_CopyResource(hw->device_context, (ID3D11Resource*)packed_readback,
                                     (ID3D11Resource*)packed_buffer);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(hw->device_context, (ID3D11Resource*)packed_readback,
                                        0, D3D11_MAP_READ, 0, &m))) {
      memcpy(output->data[0], m.pData, (size_t)output->width * (output->height + output->height / 2));
      ID3D11DeviceContext_Unmap(hw->device_context, (ID3D11Resource*)packed_readback, 0);
      delivery = 3;
    } else {
      stop_linear_output();
    }
  }
  if (!delivery && prepare_target(hw, (UINT)output->width, (UINT)output->height)) {
    render_nv12(hw->device_context, source, crop, target_view,
                bridged ? HDR_SHADER_BRIDGE : HDR_SHADER_TEXTURE, inverse);
    if (finish_output(hw, output)) delivery = target_direct ? 2 : 1;
  }
  restore_compute(hw->device_context, &previous);
  if(delivery) {
    // Keep the allocation, but return fresh frame properties on every transfer.
    if(reuse)for(int i=0;i<2;++i) {
      output->buf[i]=dst->buf[i];
      dst->buf[i]=NULL;
    }
    frame_unref(dst);
    frame_move(dst,output);
  }
unlock:
  hw->unlock(hw->lock_ctx);
release_output:
  frame_free(&output);
release_lock:
  ReleaseSRWLockExclusive(&renderer_lock);
  return delivery;
}

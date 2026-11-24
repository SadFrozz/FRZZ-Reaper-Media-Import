#include <cstdint>
#include <cmath>
#include <memory>
#include <mutex>
#include <cstdlib>
#include "platform.h"
#include "video_hdr_symbols.h"
#include "support.h"
#include <utility>
#include <d3d9.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <dxgi.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext_dxva2.h>
#include <libavutil/pixfmt.h>
}

using Microsoft::WRL::ComPtr;

static const unsigned pattern[][3]={
    {64,512,512},{940,512,512},{64,64,64},{64,960,960},
    {940,64,64},{940,960,960},{512,64,960},{512,960,64},
    {110,450,590},{210,600,430},{350,720,256},{660,272,732},
    {512,64,512},{512,512,960},
};

static ComPtr<IDirect3DSurface9> make_pattern(IDirect3DDevice9* device,D3DFORMAT format)
{
    constexpr unsigned width=64*(sizeof(pattern)/sizeof(*pattern)),height=64;
    ComPtr<IDirect3DSurface9> source;
    HRESULT status=device->CreateOffscreenPlainSurface(width,height,format,D3DPOOL_DEFAULT,&source,nullptr);
    if(FAILED(status))return {};
    D3DLOCKED_RECT pixels{};
    status=source->LockRect(&pixels,nullptr,0);
    if(FAILED(status))return {};
    auto* data=static_cast<uint8_t*>(pixels.pBits);
    for(unsigned y=0;y<height;++y) {
        auto* line=reinterpret_cast<uint16_t*>(data+size_t(y)*pixels.Pitch);
        for(unsigned x=0;x<width;++x)line[x]=static_cast<uint16_t>(pattern[x/64][0]<<6);
    }
    for(unsigned y=height;y<height*3/2;++y) {
        auto* line=reinterpret_cast<uint16_t*>(data+size_t(y)*pixels.Pitch);
        for(unsigned x=0;x<width;++x)line[x]=static_cast<uint16_t>(pattern[x/64][1+x%2]<<6);
    }
    source->UnlockRect();
    return source;
}

static bool wait_for_copy(IDirect3DQuery9* fence)
{
    if(FAILED(fence->Issue(D3DISSUE_END)))return false;
    const auto deadline=GetTickCount64()+250;
    HRESULT result=fence->GetData(nullptr,0,D3DGETDATA_FLUSH);
    while(result==S_FALSE && GetTickCount64()<deadline){
        SwitchToThread();
        result=fence->GetData(nullptr,0,D3DGETDATA_FLUSH);
    }
    return result==S_OK;
}

static bool measure_matrix(IDirect3DDevice9* video,IDirect3DQuery9* fence,ID3D11Device* device,
                           ID3D11DeviceContext* context,D3DFORMAT format,float (&transform)[12])
{
    auto test=make_pattern(video,format);
    D3DSURFACE_DESC shape{};
    if(!test || FAILED(test->GetDesc(&shape)))return false;
    ComPtr<IDirect3DTexture9> shared;
    ComPtr<IDirect3DSurface9> surface;
    ComPtr<ID3D11Texture2D> imported,download;
    HANDLE key{};
    if(FAILED(video->CreateTexture(shape.Width,shape.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,
                                   D3DPOOL_DEFAULT,&shared,&key)) || !key ||
       FAILED(shared->GetSurfaceLevel(0,&surface)) || FAILED(device->OpenSharedResource(key,IID_PPV_ARGS(&imported))) ||
       FAILED(video->StretchRect(test.Get(),nullptr,surface.Get(),nullptr,D3DTEXF_NONE)) || !wait_for_copy(fence))return false;
    D3D11_TEXTURE2D_DESC desc{};
    imported->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;
    desc.BindFlags=desc.MiscFlags=0;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&download)))return false;
    context->CopyResource(download.Get(),imported.Get());
    D3D11_MAPPED_SUBRESOURCE data{};
    if(FAILED(context->Map(download.Get(),0,D3D11_MAP_READ,0,&data)))return false;
    constexpr unsigned count=sizeof(pattern)/sizeof(*pattern);
    double rgb[count][3];
    const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(data.pData)+size_t(32)*data.RowPitch);
    for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c)
        rgb[i][c]=DirectX::PackedVector::XMConvertHalfToFloat(row[(i*64+32)*4+c]);
    context->Unmap(download.Get(),0);

    // Fit the inverse matrix, including the offset, from reference colors on this device.
    double system[4][7]{};
    for(unsigned i=0;i<count;++i){
        const double in[]={rgb[i][0],rgb[i][1],rgb[i][2],1.0};
        for(unsigned r=0;r<4;++r){
            for(unsigned c=0;c<4;++c)system[r][c]+=in[r]*in[c];
            for(unsigned c=0;c<3;++c)system[r][c+4]+=in[r]*pattern[i][c]/1024.0;
        }
    }
    for(unsigned r=0;r<4;++r){
        unsigned pivot=r;
        for(unsigned q=r+1;q<4;++q)if(std::abs(system[q][r])>std::abs(system[pivot][r]))pivot=q;
        if(std::abs(system[pivot][r])<1e-9)return false;
        for(unsigned c=0;c<7;++c)std::swap(system[pivot][c],system[r][c]);
        const double factor=system[r][r];
        for(auto& v:system[r])v/=factor;
        for(unsigned q=0;q<4;++q){
            if(q==r)continue;
            const double slope=system[q][r];
            for(unsigned c=0;c<7;++c)system[q][c]-=slope*system[r][c];
        }
    }
    for(unsigned q=0;q<count;++q)for(unsigned c=0;c<3;++c){
        const double code=1024*(rgb[q][0]*system[0][c+4]+rgb[q][1]*system[1][c+4]+
                                rgb[q][2]*system[2][c+4]+system[3][c+4]);
        if(!std::isfinite(code) || std::abs(code-pattern[q][c])>0.75)return false;
    }
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)transform[r*3+c]=static_cast<float>(system[r][c+4]);
    return true;
}

static bool video_adapter_luid(IDirect3DDevice9* video,LUID& luid)
{
    D3DDEVICE_CREATION_PARAMETERS config{};
    ComPtr<IDirect3D9> direct3d;
    ComPtr<IDirect3D9Ex> extended;
    if (FAILED(video->GetCreationParameters(&config)) || FAILED(video->GetDirect3D(&direct3d)) ||
        FAILED(direct3d.As(&extended)) || FAILED(extended->GetAdapterLUID(config.AdapterOrdinal,&luid))) return false;
    return true;
}

static ComPtr<IDXGIAdapter> adapter_for_luid(const LUID& luid)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    for (unsigned index=0;;++index) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(index,&adapter))) return {};
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc)) && desc.AdapterLuid.HighPart==luid.HighPart &&
            desc.AdapterLuid.LowPart==luid.LowPart) return adapter;
    }
}

struct SharedSurface {
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface;
    ComPtr<ID3D11Texture2D> view;
    UINT width{},height{};
};

static bool make_shared_surface(IDirect3DDevice9* decoder,ID3D11Device* device,
                                UINT width,UINT height,SharedSurface& result)
{
    HANDLE key{};
    SharedSurface next;
    if(FAILED(decoder->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,
                                     D3DPOOL_DEFAULT,&next.texture,&key)) || !key ||
       FAILED(next.texture->GetSurfaceLevel(0,&next.surface)) ||
       FAILED(device->OpenSharedResource(key,IID_PPV_ARGS(&next.view))))return false;
    next.width=width;
    next.height=height;
    result=std::move(next);
    return true;
}

struct DevicePreparation {
    LUID adapter{};
    UINT ordinal{};
    std::uint64_t started{};
    std::uint64_t completed{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HANDLE ready{};
    HANDLE calibration_ready{};
    ComPtr<IDirect3DDevice9> calibration_video;
    ComPtr<IDirect3DQuery9> calibration_fence;
    LUID calibration_adapter{};
    float matrix[12]{};
    DXGI_ADAPTER_DESC description{};
    bool calibrated{};
    ~DevicePreparation() {
        if(calibration_ready)CloseHandle(calibration_ready);
        if(ready)CloseHandle(ready);
    }
};

static std::mutex preparation_mutex;
static std::shared_ptr<DevicePreparation> preparation;
static bool preparation_stopped{};

static bool create_bridge_device(IDXGIAdapter* adapter,ID3D11Device** device,ID3D11DeviceContext** context)
{
    return SUCCEEDED(D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,
                                       D3D11_SDK_VERSION,device,nullptr,context));
}

static DWORD WINAPI prepare_device(void* data)
{
    std::unique_ptr<std::shared_ptr<DevicePreparation>> owner(static_cast<std::shared_ptr<DevicePreparation>*>(data));
    auto& job=**owner;
    if(auto runtime=LoadLibraryExW(L"d3d9.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using Create9Ex=HRESULT(WINAPI*)(UINT,IDirect3D9Ex**);
        const auto create=reinterpret_cast<Create9Ex>(GetProcAddress(runtime,"Direct3DCreate9Ex"));
        ComPtr<IDirect3D9Ex> direct;
        if(create && SUCCEEDED(create(D3D_SDK_VERSION,&direct)) && SUCCEEDED(direct->GetAdapterLUID(job.ordinal,&job.adapter))) {
            if(const auto adapter=adapter_for_luid(job.adapter)) {
                adapter->GetDesc(&job.description);
                create_bridge_device(adapter.Get(),&job.device,&job.context);
            }
        }
        direct.Reset();
        FreeLibrary(runtime);
    }
    job.completed=frzz::monotonic_ms();
    SetEvent(job.ready);
    return 0;
}

extern "C" void FRZZ_HDR_API(frzz_hdr_prepare_dxva2)(const char* selector) noexcept
{
    UINT ordinal=0;
    if(selector && *selector) {
        char* end{};
        if(*selector<'0' || *selector>'9')return;
        const auto index=std::strtoul(selector,&end,10);
        if(*end || index>=32)return;
        ordinal=static_cast<UINT>(index);
    }
    try {
        std::lock_guard<std::mutex> lock(preparation_mutex);
        if(preparation_stopped || (preparation && preparation->ordinal==ordinal))return;
        auto job=std::make_shared<DevicePreparation>();
        job->ordinal=ordinal;
        job->started=frzz::monotonic_ms();
        job->ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!job->ready)return;
        auto owner=std::make_unique<std::shared_ptr<DevicePreparation>>(job);
        if(!QueueUserWorkItem(prepare_device,owner.get(),WT_EXECUTELONGFUNCTION))return;
        owner.release();
        preparation=std::move(job);
    } catch(...) {}
}

static bool equal_adapter(const LUID& a,const LUID& b)
{
    return a.HighPart==b.HighPart && a.LowPart==b.LowPart;
}

static constexpr auto p010_format=static_cast<D3DFORMAT>(MAKEFOURCC('P','0','1','0'));

static DWORD WINAPI calibrate_device(void* data)
{
    std::unique_ptr<std::shared_ptr<DevicePreparation>> owner(static_cast<std::shared_ptr<DevicePreparation>*>(data));
    auto& job=**owner;
    if(WaitForSingleObject(job.ready,INFINITE)==WAIT_OBJECT_0 && job.device &&
       equal_adapter(job.adapter,job.calibration_adapter)) {
        auto* video=job.calibration_video.Get();
        job.calibrated=SUCCEEDED(video->CreateQuery(D3DQUERYTYPE_EVENT,&job.calibration_fence)) &&
            measure_matrix(video,job.calibration_fence.Get(),job.device.Get(),job.context.Get(),p010_format,job.matrix);
    }
    SetEvent(job.calibration_ready);
    return 0;
}

extern "C" void FRZZ_HDR_API(frzz_hdr_calibrate_dxva2)(const AVHWDeviceContext* device) noexcept
{
    if(!device || device->type!=AV_HWDEVICE_TYPE_DXVA2 || !device->hwctx)return;
    auto* manager=static_cast<const AVDXVA2DeviceContext*>(device->hwctx)->devmgr;
    if(!manager)return;
    try {
        std::lock_guard<std::mutex> lock(preparation_mutex);
        if(!preparation || preparation->calibration_ready)return;
        HANDLE handle{};
        if(FAILED(manager->OpenDeviceHandle(&handle)))return;
        ComPtr<IDirect3DDevice9> video;
        const HRESULT locked=manager->LockDevice(handle,&video,FALSE);
        if(SUCCEEDED(locked))manager->UnlockDevice(handle,FALSE);
        manager->CloseDeviceHandle(handle);
        D3DDEVICE_CREATION_PARAMETERS info{};
        LUID actual{};
        if(!video || FAILED(video->GetCreationParameters(&info)) ||
           !(info.BehaviorFlags&D3DCREATE_MULTITHREADED) || !video_adapter_luid(video.Get(),actual))return;

        auto owner=std::make_unique<std::shared_ptr<DevicePreparation>>(preparation);
        preparation->calibration_ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!preparation->calibration_ready)return;
        preparation->calibration_video=std::move(video);
        preparation->calibration_adapter=actual;
        if(QueueUserWorkItem(calibrate_device,owner.get(),WT_EXECUTELONGFUNCTION))owner.release();
        else {
            CloseHandle(preparation->calibration_ready);
            preparation->calibration_ready=nullptr;
            preparation->calibration_video.Reset();
        }
    } catch(...) {}
}

extern "C" void FRZZ_HDR_API(frzz_hdr_clear_dxva2_preparation)() noexcept
{
    std::lock_guard<std::mutex> lock(preparation_mutex);
    preparation_stopped=true;
    preparation.reset();
}

static std::shared_ptr<DevicePreparation> take_prepared_device(const LUID& actual)
{
    std::shared_ptr<DevicePreparation> job;
    {
        std::lock_guard<std::mutex> lock(preparation_mutex);
        job=preparation;
    }
    if(!job)return {};
    const auto requested=frzz::monotonic_ms();
    if(WaitForSingleObject(job->ready,120)!=WAIT_OBJECT_0)return {};
    if(!job->device) {
        std::lock_guard<std::mutex> lock(preparation_mutex);
        if(preparation==job)preparation.reset();
        return {};
    }
    if(!equal_adapter(job->adapter,actual)) {
        frzz::log("DXVA2 HDR prepared device rejected on LUID mismatch");
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(preparation_mutex);
        if(preparation_stopped)return {};
        // A decoder on another adapter may be preparing concurrently. Claim
        // the job only after its D3D9 adapter LUID matches this decoder.
        if(preparation==job)preparation.reset();
    }
    if(job->calibration_ready && WaitForSingleObject(job->calibration_ready,120)!=WAIT_OBJECT_0)return {};
    frzz::log("DXVA2 HDR D3D11 prepared in %llu ms, requested %llu ms after queue, waited %llu ms, calibration=%d",
        static_cast<unsigned long long>(job->completed-job->started),
        static_cast<unsigned long long>(requested-job->started),
        static_cast<unsigned long long>(frzz::monotonic_ms()-requested),job->calibrated);
    return job;
}

static bool revisit_redundant_preparation(IDirect3DDevice9* video,const LUID& adapter)
{
    std::shared_ptr<DevicePreparation> redundant;
    {
        std::lock_guard<std::mutex> lock(preparation_mutex);
        if(!preparation)return false;
        const auto* owner=preparation->calibration_video.Get();
        if(owner && owner!=video)return false;
        const auto state=WaitForSingleObject(preparation->ready,0);
        if(state==WAIT_TIMEOUT)return true;
        if(state!=WAIT_OBJECT_0 || (preparation->device && !equal_adapter(preparation->adapter,adapter)))return false;
        redundant=std::move(preparation);
    }
    return false;
}

extern "C" int FRZZ_HDR_API(frzz_hdr_share_dxva2)(const AVFrame* frame,AVD3D11VADeviceContext** hw,
                                    ID3D11Texture2D** input,const float** inverse)
{
    if (!frame || frame->format!=AV_PIX_FMT_DXVA2_VLD || !frame->data[3])return -1;
    auto* decoded=reinterpret_cast<IDirect3DSurface9*>(frame->data[3]);
    D3DSURFACE_DESC shape{};
    ComPtr<IDirect3DDevice9> video;
    if (FAILED(decoded->GetDesc(&shape)) || FAILED(decoded->GetDevice(&video)) ||
        shape.Format!=p010_format)return -2;
    struct Bridge {
        ComPtr<IDirect3DDevice9> video;
        SharedSurface shared;
        ComPtr<IDirect3DQuery9> fence;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        DXGI_ADAPTER_DESC description{};
        bool revisit_preparation{};
        AVD3D11VADeviceContext hw{};
        float transform[12]{};
    };
    static Bridge bridge;
    const bool first=bridge.video.Get()!=video.Get();
    const auto share_frame=[&](Bridge& state) {
        const auto surface_begin=first?frzz::monotonic_ms():0;
        SharedSurface replacement;
        auto* shared=&state.shared;
        if(!shared->view || shared->width!=shape.Width || shared->height!=shape.Height) {
            if(!make_shared_surface(video.Get(),state.device.Get(),shape.Width,shape.Height,replacement))return -7;
            shared=&replacement;
        }
        const auto copy_begin=first?frzz::monotonic_ms():0;
        if(FAILED(video->StretchRect(decoded,nullptr,shared->surface.Get(),nullptr,D3DTEXF_NONE)))return -8;
        if(!wait_for_copy(state.fence.Get()))return -9;
        if(shared==&replacement)state.shared=std::move(replacement);
        if(first)frzz::log("DXVA2 HDR surface setup %llu ms, GPU handoff %llu ms",
            static_cast<unsigned long long>(copy_begin-surface_begin),
            static_cast<unsigned long long>(frzz::monotonic_ms()-copy_begin));
        return 0;
    };
    if(first) {
        LUID actual{};
        if(!video_adapter_luid(video.Get(),actual))return -4;
        const bool reuse=bridge.device && equal_adapter(bridge.description.AdapterLuid,actual) &&
            SUCCEEDED(bridge.device->GetDeviceRemovedReason());
        std::shared_ptr<DevicePreparation> prepared;
        Bridge next;
        if(reuse) {
            next.device=bridge.device;
            next.context=bridge.context;
            next.description=bridge.description;
        } else {
            prepared=take_prepared_device(actual);
            if(prepared) {
                next.description=prepared->description;
                next.device=std::move(prepared->device);
                next.context=std::move(prepared->context);
            } else {
                const auto adapter=adapter_for_luid(actual);
                if(!adapter || FAILED(adapter->GetDesc(&next.description)) ||
                   !create_bridge_device(adapter.Get(),&next.device,&next.context))return -5;
            }
        }
        const bool calibrated=prepared && prepared->calibrated && prepared->calibration_video.Get()==video.Get();
        if(calibrated) {
            next.fence=std::move(prepared->calibration_fence);
            for(unsigned i=0;i<12;++i)next.transform[i]=prepared->matrix[i];
        } else if(FAILED(video->CreateQuery(D3DQUERYTYPE_EVENT,&next.fence)) ||
                  !measure_matrix(video.Get(),next.fence.Get(),next.device.Get(),next.context.Get(),shape.Format,next.transform))return -10;
        // D3D9 surfaces, fences and color calibration belong to the decoder's
        // device. Retaining only the same-adapter D3D11 device keeps libplacebo's
        // compiled shaders alive while those decoder resources are replaced.
        next.video=video;
        next.hw.device=next.device.Get();
        next.hw.device_context=next.context.Get();
        // This standalone D3D11 context is used only under the renderer lock.
        next.hw.lock=[](void*){};
        next.hw.unlock=[](void*){};
        if(const int result=share_frame(next);result<0)return result;
        next.revisit_preparation=!prepared && revisit_redundant_preparation(video.Get(),actual);
        bridge=std::move(next);
        frzz::log("DXVA2 HDR bridge adapter=%04x:%04x LUID=%08lx:%08lx background=%d calibration=%d dimensions=%ux%u reused_d3d11=%d",
            bridge.description.VendorId,bridge.description.DeviceId,static_cast<unsigned long>(actual.HighPart),
            static_cast<unsigned long>(actual.LowPart),prepared!=nullptr,calibrated,shape.Width,shape.Height,reuse);
    } else {
        if(bridge.revisit_preparation)
            bridge.revisit_preparation=revisit_redundant_preparation(video.Get(),bridge.description.AdapterLuid);
        if(const int result=share_frame(bridge);result<0)return result;
    }
    *hw=&bridge.hw;
    *input=bridge.shared.view.Get();
    *inverse=bridge.transform;
    return 0;
}

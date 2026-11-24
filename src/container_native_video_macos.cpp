#if defined(__APPLE__) && defined(__x86_64__)
#include "support.h"
#include "container_native_timing.h"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach/i386/thread_status.h>
#include <sys/mman.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace frzz {
extern "C" {
extern void* frzz_original_open;
extern void* frzz_original_peak_name;
extern void* frzz_original_channels;
void frzz_video_decoder_hook();
void frzz_video_channels_hook();
void frzz_video_peak_hook();
void frzz_video_peak_plain_hook();
void frzz_video_peak_legacy_hook();
void* frzz_original_peak_plain{};
void* frzz_original_peak_legacy{};
}
namespace {
const mach_header* native_video_image{};
std::atomic<const NativeTimingHook*> active_audio_timing{};
const NativeTimingHook* active_hardware_timing{};
unsigned audio_open_return{};
constexpr size_t jump_size=14;
constexpr unsigned char factory_prologue[]{0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x53,0x55,0x48,0x83,0xec,0x28};
constexpr unsigned char copy_channel_count[]{
  0x89,0x4d,0x44,0x8b,0x8e,0x5c,0x01,0,0,0xff,0xc1,0x8b,0x96,0x58,0x01,0,0};
constexpr unsigned char legacy_copy_channel_count[]{
  0x89,0x4d,0x44,0x8b,0x8e,0x68,0x01,0,0,0xff,0xc1,0x8b,0x96,0x60,0x01,0,0};
struct NativeFactory {
  unsigned decoder;
  unsigned channel_count;
  unsigned format_open;
  unsigned open_return;
};
// REAPER 7.85 Intel has a separate native factory for each FFmpeg ABI.
constexpr NativeFactory factories[]{
  {0x167490,0x1678aa,0x238508,0x16767c},
  {0x173a10,0x173e21,0x238748,0x173bfc},
  {0x180000,0x180411,0x2389a8,0x1801ec},
  {0x18c5f0,0x18ca01,0x238c08,0x18c7dc},
  {0x198c50,0x199061,0x238e68,0x198e3c},
};
constexpr unsigned char peak_prologue[]{
  0x55,0x48,0x89,0xe5,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x53,0x48,0x81,0xec,0x58,0x0f,0x00,0x00};

std::string file_sha256(const char* path) {
  if(!path)return {};
  std::ifstream file(path,std::ios::binary|std::ios::ate);
  const auto length=file.tellg();
  if(length<=0 || length>64*1024*1024)return {};
  std::vector<unsigned char> bytes(static_cast<size_t>(length));
  file.seekg(0);
  if(!file.read(reinterpret_cast<char*>(bytes.data()),length))return {};
  unsigned char hash[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(bytes.data(),static_cast<CC_LONG>(bytes.size()),hash);
  constexpr char digits[]="0123456789abcdef";
  std::string result;
  for(auto byte:hash){result+=digits[byte>>4];result+=digits[byte&15];}
  return result;
}

struct Hook {
  unsigned char* entry;
  const unsigned char* prologue;
  size_t length;
  void* replacement;
  void** original;
  vm_address_t page() const {return reinterpret_cast<vm_address_t>(entry)&~(vm_page_size-1);}
};
using Hooks=std::array<Hook,5>;

void write_jump(unsigned char* bytes,void* destination) {
  constexpr unsigned char instruction[]{0xff,0x25,0,0,0,0};
  std::memcpy(bytes,instruction,sizeof(instruction));
  std::memcpy(bytes+sizeof(instruction),&destination,sizeof(destination));
}

template <typename Function>
auto mach_function(const char* name) {
  return reinterpret_cast<Function>(dlsym(RTLD_DEFAULT,name));
}

struct MachCalls {
  const decltype(&task_threads) list=mach_function<decltype(&task_threads)>("task_threads");
  const decltype(&thread_suspend) suspend=mach_function<decltype(&thread_suspend)>("thread_suspend");
  const decltype(&thread_resume) resume=mach_function<decltype(&thread_resume)>("thread_resume");
  const decltype(&thread_get_state) state=mach_function<decltype(&thread_get_state)>("thread_get_state");
  const decltype(&mach_port_deallocate) release=mach_function<decltype(&mach_port_deallocate)>("mach_port_deallocate");
  const decltype(&vm_deallocate) deallocate=mach_function<decltype(&vm_deallocate)>("vm_deallocate");
  const decltype(&vm_protect) protect=mach_function<decltype(&vm_protect)>("vm_protect");
  const decltype(&vm_remap) remap=mach_function<decltype(&vm_remap)>("vm_remap");
  bool available() const {return list && suspend && resume && state && release && deallocate && protect && remap;}
};

class SuspendedThreads {
  const MachCalls& calls;
  thread_act_array_t threads{};
  mach_msg_type_number_t count{};
  mach_msg_type_number_t checked{};
  const thread_t self=mach_thread_self();
public:
  explicit SuspendedThreads(const MachCalls& calls):calls(calls){}
  ~SuspendedThreads() {
    for(mach_msg_type_number_t i=0;i<count;++i) {
      if(i<checked && threads[i]!=self)calls.resume(threads[i]);
      calls.release(mach_task_self(),threads[i]);
    }
    if(threads)calls.deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(threads),count*sizeof(thread_t));
    calls.release(mach_task_self(),self);
  }
  bool stop(const Hooks& hooks) {
    if(calls.list(mach_task_self(),&threads,&count)!=KERN_SUCCESS)return false;
    while(checked<count) {
      const auto thread=threads[checked];
      if(thread!=self && calls.suspend(thread)!=KERN_SUCCESS)return false;
      ++checked;
      if(thread==self)continue;
      x86_thread_state64_t state{};
      mach_msg_type_number_t size=x86_THREAD_STATE64_COUNT;
      if(calls.state(thread,x86_THREAD_STATE64,reinterpret_cast<thread_state_t>(&state),&size)!=KERN_SUCCESS)return false;
      for(const auto& hook:hooks) {
        const auto entry=reinterpret_cast<std::uintptr_t>(hook.entry);
        if(state.__rip>=entry && state.__rip<entry+hook.length)return false;
      }
    }
    // A host thread could have created another one before it was suspended.
    thread_act_array_t current{};
    mach_msg_type_number_t current_count{};
    if(calls.list(mach_task_self(),&current,&current_count)!=KERN_SUCCESS)return false;
    bool stable=true;
    for(mach_msg_type_number_t i=0;i<current_count;++i) {
      bool found=false;
      for(mach_msg_type_number_t j=0;j<count;++j)found|=current[i]==threads[j];
      stable&=found;
      calls.release(mach_task_self(),current[i]);
    }
    calls.deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(current),current_count*sizeof(thread_t));
    return stable;
  }
};

bool patch(const Hooks& hooks) {
  static bool protection_failed=false;
  // Resolve Mach calls before suspending threads that might hold dyld locks.
  static const MachCalls calls;
  if(protection_failed || !calls.available())return false;
  // One executable allocation holds all displaced prologues. Their exact
  // bytes are checked before copying and contain no relative instructions.
  std::array<vm_address_t,std::tuple_size_v<Hooks>> pages{};
  std::array<std::size_t,std::tuple_size_v<Hooks>> hook_pages{};
  std::size_t page_count=0;
  for(std::size_t i=0;i<hooks.size();++i) {
    const auto page=hooks[i].page();
    std::size_t index=0;
    while(index<page_count && pages[index]!=page)++index;
    if(index==page_count)pages[page_count++]=page;
    hook_pages[i]=index;
  }
  const auto length=(1+page_count)*vm_page_size;
  auto* memory=static_cast<unsigned char*>(mmap(nullptr,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0));
  if(memory==MAP_FAILED)return false;
  size_t offset=0;
  for(size_t i=0;i<hooks.size();++i) {
    const auto& hook=hooks[i];
    std::memcpy(memory+offset,hook.prologue,hook.length);
    write_jump(memory+offset+hook.length,hook.entry+hook.length);
    *hook.original=memory+offset;
    offset+=hook.length+jump_size;
  }
  if(mprotect(memory,vm_page_size,PROT_READ|PROT_EXEC)!=0) {
    protection_failed=true;
    munmap(memory,length);
    return false;
  }

  size_t installed=0;
  {
    SuspendedThreads threads(calls);
    if(threads.stop(hooks)) {
      const auto task=mach_task_self();
      bool expected=true;
      for(std::size_t i=0;i<page_count;++i)
        std::memcpy(memory+(1+i)*vm_page_size,reinterpret_cast<void*>(pages[i]),vm_page_size);
      for(size_t i=0;i<hooks.size();++i) {
        const auto& hook=hooks[i];
        expected&=std::memcmp(hook.entry,hook.prologue,hook.length)==0;
        auto* staged=memory+(1+hook_pages[i])*vm_page_size;
        write_jump(staged+(reinterpret_cast<vm_address_t>(hook.entry)-hook.page()),hook.replacement);
      }
      if(expected && calls.protect(task,reinterpret_cast<vm_address_t>(memory+vm_page_size),length-vm_page_size,
                                  false,VM_PROT_READ|VM_PROT_EXECUTE)==KERN_SUCCESS) {
        for(;installed<page_count;++installed) {
          auto target=pages[installed];
          const auto staged=reinterpret_cast<vm_address_t>(memory+(1+installed)*vm_page_size);
          vm_prot_t current{},maximum{};
          if(calls.remap(task,&target,vm_page_size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,task,staged,true,
                         &current,&maximum,VM_INHERIT_COPY)!=KERN_SUCCESS)break;
        }
      }
      protection_failed=installed!=page_count;
    }
  }
  // Keep any installed trampolines valid even if another page could not be
  // remapped. The routing stays disabled until every hook is in place.
  if(installed)munmap(memory+vm_page_size,length-vm_page_size);
  else munmap(memory,length);
  return installed==page_count;
}

struct VideoPlugin {
  const mach_header* image{};
  const char* path{};
};

VideoPlugin find_video_plugin() {
  for(uint32_t i=0;i<_dyld_image_count();++i) {
    const char* name=_dyld_get_image_name(i);
    if(!name)continue;
    const std::string_view path(name);
    const auto filename=path.substr(path.find_last_of('/')+1);
    if(filename=="reaper_video.dylib")return {_dyld_get_image_header(i),name};
  }
  return {};
}

bool pin_video_plugin(const VideoPlugin& video) {
  // Keep FFmpeg dispatch slots and late callbacks valid after deactivation.
  static const mach_header* pinned{};
  if(pinned)return pinned==video.image;
  Dl_info self{};
  if(!dladdr(reinterpret_cast<void*>(&frzz_video_decoder_hook),&self))return false;
  constexpr int flags=RTLD_LAZY|RTLD_NOLOAD|RTLD_NODELETE;
  if(!dlopen(video.path,flags) || !dlopen(self.dli_fname,flags))return false;
  pinned=video.image;
  return true;
}
}

bool install_macos_video_hooks(void* peak_name,void* peak_ex,void* peak_legacy) {
  const auto plugin=find_video_plugin();
  const auto* video=plugin.image;
  if(!video)return false;
  static const bool supported=
    file_sha256(plugin.path)=="171310744044a73442012477151d57498b1221c51f2fbbf921a73de8186084f6" &&
    file_sha256(_dyld_get_image_name(0))=="5ab37839e505b045c2eea17a60c103ecabeecda3a51d90ad86fdb67521dc3381";
  if(!supported)return false;
  auto* image=reinterpret_cast<unsigned char*>(const_cast<mach_header*>(video));
  const NativeTimingHook* timing=active_hardware_timing;
  for(const auto* adapter:native_timing_adapters) {
    if(timing)break;
    const auto format=retain_loaded_module("avformat",adapter->format_major);
    if(!format)continue;
    const auto* open=module_symbol(format,"avformat_open_input");
    release_module(format);
    const auto& factory=factories[adapter->format_major-59];
    if(open && *reinterpret_cast<void**>(image+factory.format_open)==open) {
      timing=adapter;
      break;
    }
  }
  if(!timing)return false;
  const auto& factory=factories[timing->format_major-59];
  const auto* channel_prologue=timing->format_major<=60?legacy_copy_channel_count:copy_channel_count;
  const Hooks hooks{{
    {image+factory.decoder,factory_prologue,sizeof(factory_prologue),reinterpret_cast<void*>(&frzz_video_decoder_hook),&frzz_original_open},
    {static_cast<unsigned char*>(peak_name),peak_prologue,sizeof(peak_prologue),reinterpret_cast<void*>(&frzz_video_peak_hook),&frzz_original_peak_name},
    {static_cast<unsigned char*>(peak_ex),peak_prologue,sizeof(peak_prologue),reinterpret_cast<void*>(&frzz_video_peak_plain_hook),&frzz_original_peak_plain},
    {static_cast<unsigned char*>(peak_legacy),peak_prologue,sizeof(peak_prologue),reinterpret_cast<void*>(&frzz_video_peak_legacy_hook),&frzz_original_peak_legacy},
    {image+factory.channel_count,channel_prologue,sizeof(copy_channel_count),
      reinterpret_cast<void*>(&frzz_video_channels_hook),&frzz_original_channels},
  }};
  for(size_t i=0;i<hooks.size();++i) {
    const auto& hook=hooks[i];
    if(!hook.entry || reinterpret_cast<vm_address_t>(hook.entry)-hook.page()+hook.length>vm_page_size ||
       std::memcmp(hook.entry,hook.prologue,hook.length)!=0) {
      log("native container track routing: FFmpeg ABI %d hook %zu did not match",
          timing->format_major,i);
      return false;
    }
  }

  if(!pin_video_plugin(plugin))return false;
  if(!patch(hooks)) {
    log("native container track routing: could not enable FFmpeg ABI %d hooks",timing->format_major);
    return false;
  }
  native_video_image=video;
  audio_open_return=factory.open_return;
  active_audio_timing=timing;
  return true;
}

const NativeTimingHook* macos_video_timing() noexcept {
  return active_audio_timing.load();
}

bool install_macos_video_timing() {
  const auto* timing=macos_video_timing();
  return timing && timing->install({const_cast<mach_header*>(native_video_image),0,audio_open_return});
}

bool install_macos_hardware_video() {
  const auto video=find_video_plugin();
  if(!video.image || !pin_video_plugin(video))return false;
  const NativeTimingSlots slots{const_cast<mach_header*>(video.image),0,0};
  for(const auto* timing:native_timing_adapters) {
    if(timing->install(slots)) {
      active_hardware_timing=timing;
      return true;
    }
  }
  return false;
}

}
#endif

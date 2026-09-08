#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <cstring>
#endif
namespace frzz {
#ifdef _WIN32
using ModuleHandle=HMODULE;
inline void* module_symbol(ModuleHandle module,const char* name){return reinterpret_cast<void*>(GetProcAddress(module,name));}
inline void release_module(ModuleHandle module){if(module)FreeLibrary(module);}
inline ModuleHandle retain_loaded_module(const char* family,int major) {
  const auto name=std::wstring(family,family+strlen(family))+L"-"+std::to_wstring(major)+L".dll";
  HMODULE module{};return GetModuleHandleExW(0,name.c_str(),&module)?module:nullptr;
}
#else
using ModuleHandle=void*;
inline void* module_symbol(ModuleHandle module,const char* name){return dlsym(module,name);}
inline void release_module(ModuleHandle module){if(module)dlclose(module);}
inline ModuleHandle retain_loaded_module(const char* family,int major) {
  const std::string prefix=std::string("lib")+family+"."+std::to_string(major);
  for(uint32_t i=0;i<_dyld_image_count();++i) {
    const char* path=_dyld_get_image_name(i);if(!path)continue;
    const char* base=strrchr(path,'/');base=base?base+1:path;
    if(!strncmp(base,prefix.c_str(),prefix.size()) && (base[prefix.size()]=='.' || !strcmp(base+prefix.size(),".dylib"))) {
      if(auto module=dlopen(path,RTLD_LAZY|RTLD_LOCAL|RTLD_NOLOAD))return module;
    }
  }
  return nullptr;
}
#endif
inline std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
using ThreadId=std::thread::id;
inline ThreadId current_thread_id(){return std::this_thread::get_id();}
inline std::size_t numeric_thread_id(){return std::hash<ThreadId>{}(current_thread_id());}
}

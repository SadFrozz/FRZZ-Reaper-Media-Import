#include "video_hdr_shader_cache.h"
#include "video_hdr_d3d11_shader.h"
#include "support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Bytecode = std::vector<std::uint8_t>;
using ShaderSet = std::array<Bytecode, HDR_SHADER_COUNT>;
constexpr std::size_t shader_size_limit = 1024 * 1024;

std::atomic<bool> cache_started{};
std::atomic<unsigned> dovi_shader_profile{};
std::atomic<bool> stopping{};
std::mutex shader_mutex;
ShaderSet shaders;
std::thread startup;
fs::path cache_path;
std::uint64_t graphics_key{};

std::uint64_t hash_bytes(const void* data, std::size_t size, std::uint64_t hash = 14695981039346656037ull) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < size; ++i)
    hash = (hash ^ bytes[i]) * 1099511628211ull;
  return hash;
}

std::uint64_t format_key() {
  const auto color_hash = hash_bytes(hdr_d3d11_color_source, sizeof(hdr_d3d11_color_source));
  return hash_bytes(hdr_d3d11_output_source, sizeof(hdr_d3d11_output_source), color_hash);
}

std::uint64_t graphics_environment(std::vector<ComPtr<IDXGIAdapter1>>& hardware) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
  std::vector<std::array<std::uint64_t, 5>> adapters;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const auto result = factory->EnumAdapters1(index, &adapter);
    if (result == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(result)) return 0;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) return 0;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    LARGE_INTEGER driver{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)) || !driver.QuadPart)
      return 0;
    adapters.push_back({desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision,
                        static_cast<std::uint64_t>(driver.QuadPart)});
    hardware.push_back(std::move(adapter));
  }
  if (adapters.empty()) return 0;
  std::sort(adapters.begin(), adapters.end());
  return hash_bytes(adapters.data(), adapters.size() * sizeof(adapters.front()));
}

bool read_cache(ShaderSet& loaded) {
  std::ifstream file(cache_path, std::ios::binary);
  std::uint64_t keys[2]{};
  if (!file.read(reinterpret_cast<char*>(keys), sizeof(keys)) ||
      keys[0] != format_key() || keys[1] != graphics_key)
    return false;
  for (auto& blob : loaded) {
    std::uint64_t fields[2]{};
    if (!file.read(reinterpret_cast<char*>(fields), sizeof(fields)) || !fields[0] || fields[0] > shader_size_limit)
      return false;
    blob.resize(static_cast<std::size_t>(fields[0]));
    if (!file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size())) ||
        hash_bytes(blob.data(), blob.size()) != fields[1])
      return false;
  }
  return file.peek() == std::ifstream::traits_type::eof();
}

void write_cache(const ShaderSet& compiled) {
  if (cache_path.empty()) return;
  fs::create_directories(cache_path.parent_path());
  auto temporary = cache_path;
  temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  const std::uint64_t keys[] = {format_key(), graphics_key};
  file.write(reinterpret_cast<const char*>(keys), sizeof(keys));
  for (const auto& blob : compiled) {
    const std::uint64_t fields[] = {blob.size(), hash_bytes(blob.data(), blob.size())};
    file.write(reinterpret_cast<const char*>(fields), sizeof(fields));
    file.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
  }
  file.close();
  if (!file || !MoveFileExW(temporary.c_str(), cache_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    DeleteFileW(temporary.c_str());
}

bool compile_shader(HdrShader id, Bytecode& bytecode) {
  const std::string_view source = id == HDR_SHADER_COLOR ? hdr_d3d11_color_source : hdr_d3d11_output_source;
  const D3D_SHADER_MACRO defines[] = {
      {"HDR_PACKED", id == HDR_SHADER_PACKED ? "1" : "0"},
      {"HDR_BRIDGED", id == HDR_SHADER_BRIDGE ? "1" : "0"},
      {nullptr, nullptr},
  };
  ComPtr<ID3DBlob> shader, errors;
  const auto result = D3DCompile(source.data(), source.size(), "FRZZ HDR", defines, nullptr, "main", "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
  if (FAILED(result)) {
    frzz::log("HDR shader compilation failed (%u): %s", static_cast<unsigned>(id),
              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler output");
    return false;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(shader->GetBufferPointer());
  bytecode.assign(bytes, bytes + shader->GetBufferSize());
  return !bytecode.empty();
}

void prepare_cache(std::vector<ComPtr<IDXGIAdapter1>> hardware) noexcept {
  try {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ShaderSet compiled;
    {
      std::lock_guard<std::mutex> lock(shader_mutex);
      for (unsigned i = 0; i < HDR_SHADER_COUNT; ++i) {
        if (stopping.load(std::memory_order_relaxed)) return;
        if (shaders[i].empty() && !compile_shader(static_cast<HdrShader>(i), shaders[i])) return;
      }
      compiled = shaders;
    }
    for (const auto& adapter : hardware) {
      if (stopping.load(std::memory_order_relaxed)) return;
      ComPtr<ID3D11Device> device;
      if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, nullptr))) continue;
      for (const auto& blob : compiled) {
        ComPtr<ID3D11ComputeShader> native;
        if (FAILED(device->CreateComputeShader(blob.data(), blob.size(), nullptr, &native)))
          frzz::log("HDR driver shader preparation failed");
      }
    }
    write_cache(compiled);
    frzz::log("HDR shader cache prepared");
  } catch (...) {
    frzz::log("HDR shader cache preparation failed");
  }
}
}

extern "C" ID3D11ComputeShader* frzz_hdr_create_compute_shader(ID3D11Device* device, HdrShader id) {
  try {
    std::lock_guard<std::mutex> lock(shader_mutex);
    auto& bytecode = shaders[id];
    if (bytecode.empty() && !compile_shader(id, bytecode)) return nullptr;
    ID3D11ComputeShader* shader = nullptr;
    if (SUCCEEDED(device->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader)))
      return shader;
  } catch (...) {
    frzz::log("HDR shader creation failed (%u)", static_cast<unsigned>(id));
  }
  return nullptr;
}

extern "C" void frzz_hdr_load_shader_cache(const char* resource_path) noexcept {
  if (cache_started.exchange(true, std::memory_order_acq_rel)) return;
  try {
    std::vector<ComPtr<IDXGIAdapter1>> hardware;
    graphics_key = graphics_environment(hardware);
    if (graphics_key && resource_path && *resource_path) {
      cache_path = fs::u8path(resource_path) / "UserPlugins" / "Shaders" / "frzz-hdr-d3d11.bin";
      ShaderSet loaded;
      if (read_cache(loaded)) {
        std::lock_guard<std::mutex> lock(shader_mutex);
        shaders = std::move(loaded);
        frzz::log("HDR shader cache loaded");
        return;
      }
    }
    stopping.store(false, std::memory_order_relaxed);
    startup = std::thread(prepare_cache, std::move(hardware));
  } catch (...) {
    frzz::log("HDR shader cache could not be initialized");
  }
}

extern "C" bool frzz_hdr_shader_cache_ready(void) {
  return cache_started.load(std::memory_order_acquire);
}

extern "C" void frzz_hdr_hint_dovi_shader(unsigned profile) {
  unsigned expected = 0;
  if (profile == 5 || profile == 7)
    dovi_shader_profile.compare_exchange_strong(expected, profile, std::memory_order_relaxed);
}

extern "C" unsigned frzz_hdr_warmup_dovi_profile(void) {
  return dovi_shader_profile.load(std::memory_order_relaxed);
}

extern "C" void frzz_hdr_save_shader_cache() noexcept {
  stopping.store(true, std::memory_order_relaxed);
  if (startup.joinable()) startup.join();
}

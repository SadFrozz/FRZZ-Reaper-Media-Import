#pragma once

#include <stdbool.h>
#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
void frzz_hdr_load_shader_cache(const char* resource_path) noexcept;
void frzz_hdr_save_shader_cache() noexcept;
#endif
enum HdrShader { HDR_SHADER_COLOR, HDR_SHADER_TEXTURE, HDR_SHADER_PACKED, HDR_SHADER_BRIDGE, HDR_SHADER_COUNT };
ID3D11ComputeShader* frzz_hdr_create_compute_shader(ID3D11Device* device, enum HdrShader shader);
bool frzz_hdr_shader_cache_ready(void);
void frzz_hdr_hint_dovi_shader(unsigned profile);
unsigned frzz_hdr_warmup_dovi_profile(void);
#ifdef __cplusplus
}
#endif

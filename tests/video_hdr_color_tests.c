#include "video_hdr_metadata.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define COBJMACROS
#include <d3dcompiler.h>
#include "video_hdr_d3d11_shader.h"
#endif

static void require(bool condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void check_curve(const HdrBrightness* brightness)
{
    HdrColor color = {0};
    frzz_hdr_make_tone_curve(&color, brightness);
    const float* lut = color.tone_lut;
    require(lut[0] < 0.001f, "tone mapping should keep signal black near zero");
    for (int i = 1; i < FRZZ_HDR_TONE_SAMPLES; ++i)
        require(isfinite(lut[i]) && lut[i] >= lut[i - 1], "tone mapping must be finite and monotonic");
    const float white = frzz_hdr_pq_to_nits(lut[FRZZ_HDR_TONE_SAMPLES - 1]) * color.tone_limits[2] / 10000;
    require(fabsf(white - 1.0f) < 0.0005f, "HDR peak must map onto SDR reference white");
}

struct RpuFixture {
    AVDOVIMetadata entry;
    AVDOVIRpuDataHeader header;
    AVDOVIDataMapping mapping;
    AVDOVIColorMetadata color;
};

static void check_rpu(void)
{
    struct RpuFixture rpu = {0};
    rpu.entry.header_offset = offsetof(struct RpuFixture, header);
    rpu.entry.mapping_offset = offsetof(struct RpuFixture, mapping);
    rpu.entry.color_offset = offsetof(struct RpuFixture, color);
    rpu.header.bl_bit_depth = 10;
    rpu.header.coef_log2_denom = 12;
    rpu.color.source_max_pq = 3079;
    for (unsigned i = 0; i < 3; ++i) {
        rpu.color.ycc_to_rgb_offset[i] = (AVRational){1, 4};
        for (unsigned j = 0; j < 3; ++j) {
            rpu.color.ycc_to_rgb_matrix[i * 3 + j] = (AVRational){i == j, 1};
            rpu.color.rgb_to_lms_matrix[i * 3 + j] = (AVRational){i == j, 1};
        }
        rpu.mapping.curves[i].num_pivots = 2;
        rpu.mapping.curves[i].pivots[1] = 1023;
        rpu.mapping.curves[i].poly_order[0] = 1;
        rpu.mapping.curves[i].poly_coef[0][1] = 4096;
    }
    rpu.mapping.curves[2].mapping_idc[0] = AV_DOVI_MAPPING_MMR;
    rpu.mapping.curves[2].mmr_order[0] = 3;
    rpu.mapping.curves[2].mmr_coef[0][2][6] = 2048;

    const AVFrameSideData metadata = {.data = (uint8_t*)&rpu, .size = sizeof(rpu)};
    HdrColor color = {0};
    HdrBrightness brightness = {0};
    require(hdr_read_dovi(&color, &brightness, &metadata), "valid base-layer RPU rejected");
    require(color.signal_matrix[0][3] == -0.25f, "RPU color offset lost");
    require(color.coefficients[0][0][1] == 1.0f, "polynomial normalization failed");
    require(color.mmr[2][0][2][1][2] == 0.5f, "MMR normalization failed");

    rpu.mapping.curves[2].mmr_order[0] = 4;
    require(!hdr_read_dovi(&color, &brightness, &metadata), "invalid RPU MMR order accepted");
    rpu.entry.mapping_offset = sizeof(rpu);
    require(!hdr_read_dovi(&color, &brightness, &metadata), "out-of-bounds RPU offset accepted");
}

#ifdef _WIN32
static void check_shader(const char* source, const char* packed, const char* bridge)
{
    const D3D_SHADER_MACRO defines[] = {{"HDR_PACKED", packed}, {"HDR_BRIDGED", bridge}, {NULL, NULL}};
    ID3DBlob *bytecode = NULL, *errors = NULL;
    const HRESULT result = D3DCompile(source, strlen(source), "FRZZ HDR", defines, NULL, "main", "cs_5_0",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
    if (FAILED(result) && errors)
        fputs((const char*)ID3D10Blob_GetBufferPointer(errors), stderr);
    if (errors) ID3D10Blob_Release(errors);
    if (bytecode) ID3D10Blob_Release(bytecode);
    require(SUCCEEDED(result), "D3D11 shader failed to compile");
}
#endif

int main(void)
{
    require(fabsf(frzz_hdr_pq_to_nits(0.5080784f) - 100.0f) < 0.01f, "incorrect PQ luminance");
    check_curve(&(HdrBrightness){.max_nits = 1000.0f});
    check_curve(&(HdrBrightness){.max_nits = 4000.0f, .avg_nits = 450.0f});
    check_curve(&(HdrBrightness){.max_nits = 1000.0f, .avg_nits = 350.0f, .hdr10plus = true});
    check_curve(&(HdrBrightness){.max_nits = 1000.0f, .target_nits = 600.0f, .avg_nits = 65.0f,
                                 .knee_x = 0.15f, .knee_y = 0.3f, .hdr10plus = true,
                                 .anchor_count = 3, .anchors = {0.25f, 0.65f, 0.9f}});
    check_rpu();
#ifdef _WIN32
    check_shader(hdr_d3d11_color_source, "0", "0");
    check_shader(hdr_d3d11_output_source, "0", "0");
    check_shader(hdr_d3d11_output_source, "1", "0");
    check_shader(hdr_d3d11_output_source, "0", "1");
#endif
    return 0;
}

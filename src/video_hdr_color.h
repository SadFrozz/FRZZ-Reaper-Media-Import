#pragma once

#include <stdbool.h>
#include <stddef.h>

#define FRZZ_HDR_TONE_SAMPLES 256
#define FRZZ_HDR_DOVI_PIVOTS 9
#define FRZZ_HDR_DOVI_SEGMENTS 8
#define FRZZ_HDR_DOVI_MMR_ORDER 3

// Four-float rows conform to both HLSL constant buffers and GLSL std140.
typedef struct HdrColor {
    float signal_matrix[3][4];
    float linear_matrix[3][4];
    float gamut_matrix[3][4];
    float tone_limits[4];
    float pivot_limits[3][4];
    float pivots[3][3][4];
    float coefficients[3][FRZZ_HDR_DOVI_SEGMENTS][4];
    float mmr[3][FRZZ_HDR_DOVI_SEGMENTS][FRZZ_HDR_DOVI_MMR_ORDER][2][4];
    float tone_lut[FRZZ_HDR_TONE_SAMPLES];
} HdrColor;

typedef struct HdrBrightness {
    float min_nits;
    float max_nits;
    float avg_nits;
    float target_nits;
    float knee_x;
    float knee_y;
    unsigned anchor_count;
    float anchors[15];
    bool hdr10plus;
} HdrBrightness;

#ifdef __cplusplus
extern "C" {
#endif
void frzz_hdr_make_tone_curve(HdrColor* color, const HdrBrightness* brightness);
float frzz_hdr_pq_to_nits(float pq);
#ifdef __cplusplus
}
#endif

/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */

// The spline and ST 2094-40 curve implementation is adapted from libplacebo
// v7.387.0 (275f7dea27924cdd34ab2e6c20e448b6bc4fab65), src/tone_mapping.c.
#include "video_hdr_color.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum Scaling { PL_HDR_NITS, PL_HDR_PQ };
struct pl_tone_map_params {
    enum Scaling input_scaling, output_scaling;
    size_t lut_size;
    float input_min, input_max, input_avg, output_min, output_max;
    struct {
        float knee_adaptation, knee_minimum, knee_maximum, knee_default;
        float slope_tuning, slope_offset, spline_contrast;
    } constants;
    struct {
        struct {
            float knee_x, knee_y, target_luma;
            unsigned num_anchors;
            float anchors[15];
        } ootf;
    } hdr;
};

#define fclampf(x, lo, hi) fminf(fmaxf(x, lo), hi)
#define PL_CLAMP(x, lo, hi) fclampf((float)(x), (float)(lo), (float)(hi))
#define PL_MIX(a, b, t) ((a) + ((b) - (a)) * (t))
#define PL_DEF(a, b) ((a) ? (a) : (b))
#define PL_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define pl_assert assert
#define FOREACH_LUT(lut, V) \
    for (float *_iter = lut, *_end = lut + params->lut_size, V; \
         _iter < _end && (V = *_iter, 1); *_iter++ = V)

static float pl_smoothstep(float a, float b, float x)
{
    const float t = fclampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float nits_to_pq(float nits)
{
    const float p = powf(fclampf(nits / 10000.0f, 0.0f, 1.0f), 2610.0f / 16384.0f);
    return powf((3424.0f / 4096.0f + 2413.0f / 128.0f * p) /
                (1.0f + 2392.0f / 128.0f * p), 2523.0f / 32.0f);
}

float frzz_hdr_pq_to_nits(float pq)
{
    const float p = powf(fclampf(pq, 0.0f, 1.0f), 32.0f / 2523.0f);
    return 10000.0f * powf(fmaxf(p - 3424.0f / 4096.0f, 0.0f) /
                           (2413.0f / 128.0f - 2392.0f / 128.0f * p),
                           16384.0f / 2610.0f);
}

static float pl_hdr_rescale(enum Scaling in, enum Scaling out, float value)
{
    if (in == out || value == 0.0f)
        return value;
    return out == PL_HDR_PQ ? nits_to_pq(value) : frzz_hdr_pq_to_nits(value);
}

static inline float bt1886_eotf(float x, float min, float max)
{
    const float lb = powf(min, 1/2.4f);
    const float lw = powf(max, 1/2.4f);
    return powf((lw - lb) * x + lb, 2.4f);
}

static inline float bt1886_oetf(float x, float min, float max)
{
    const float lb = powf(min, 1/2.4f);
    const float lw = powf(max, 1/2.4f);
    return (powf(x, 1/2.4f) - lb) / (lw - lb);
}

static void st2094_pick_knee(float *out_src_knee, float *out_dst_knee,
                             const struct pl_tone_map_params *params)
{
    const float src_min = pl_hdr_rescale(params->input_scaling,  PL_HDR_PQ, params->input_min);
    const float src_max = pl_hdr_rescale(params->input_scaling,  PL_HDR_PQ, params->input_max);
    const float src_avg = pl_hdr_rescale(params->input_scaling,  PL_HDR_PQ, params->input_avg);
    const float dst_min = pl_hdr_rescale(params->output_scaling, PL_HDR_PQ, params->output_min);
    const float dst_max = pl_hdr_rescale(params->output_scaling, PL_HDR_PQ, params->output_max);

    const float min_knee = params->constants.knee_minimum;
    const float max_knee = params->constants.knee_maximum;
    const float def_knee = params->constants.knee_default;
    const float src_knee_min = PL_MIX(src_min, src_max, min_knee);
    const float src_knee_max = PL_MIX(src_min, src_max, max_knee);
    const float dst_knee_min = PL_MIX(dst_min, dst_max, min_knee);
    const float dst_knee_max = PL_MIX(dst_min, dst_max, max_knee);

    // Choose source knee based on source scene brightness
    float src_knee = PL_DEF(src_avg, PL_MIX(src_min, src_max, def_knee));
    src_knee = fclampf(src_knee, src_knee_min, src_knee_max);

    // Choose target adaptation point based on linearly re-scaling source knee
    float target = (src_knee - src_min) / (src_max - src_min);
    float adapted = PL_MIX(dst_min, dst_max, target);

    // Choose the destnation knee by picking the perceptual adaptation point
    // between the source knee and the desired target. This moves the knee
    // point, on the vertical axis, closer to the 1:1 (neutral) line.
    //
    // Adjust the adaptation strength towards 1 based on how close the knee
    // point is to its extreme values (min/max knee)
    float tuning = 1.0f - pl_smoothstep(max_knee, def_knee, target) *
                          pl_smoothstep(min_knee, def_knee, target);
    float adaptation = PL_MIX(params->constants.knee_adaptation, 1.0f, tuning);

    // Brightness matching is only meaningful where the source knee is
    // displayable: below the output floor the anchor is unreachable, and
    // mixing towards it drags the knee into the black-point region,
    // crushing the shadows. Fade to pure relative adaptation there.
    adaptation = PL_MIX(1.0f, adaptation,
                        pl_smoothstep(dst_min, dst_knee_min, src_knee));

    float dst_knee = PL_MIX(src_knee, adapted, adaptation);
    dst_knee = fclampf(dst_knee, dst_knee_min, dst_knee_max);

    *out_src_knee = pl_hdr_rescale(PL_HDR_PQ, params->input_scaling, src_knee);
    *out_dst_knee = pl_hdr_rescale(PL_HDR_PQ, params->output_scaling, dst_knee);
}

// Pascal's triangle
static const uint16_t binom[17][17] = {
    {1},
    {1,1},
    {1,2,1},
    {1,3,3,1},
    {1,4,6,4,1},
    {1,5,10,10,5,1},
    {1,6,15,20,15,6,1},
    {1,7,21,35,35,21,7,1},
    {1,8,28,56,70,56,28,8,1},
    {1,9,36,84,126,126,84,36,9,1},
    {1,10,45,120,210,252,210,120,45,10,1},
    {1,11,55,165,330,462,462,330,165,55,11,1},
    {1,12,66,220,495,792,924,792,495,220,66,12,1},
    {1,13,78,286,715,1287,1716,1716,1287,715,286,78,13,1},
    {1,14,91,364,1001,2002,3003,3432,3003,2002,1001,364,91,14,1},
    {1,15,105,455,1365,3003,5005,6435,6435,5005,3003,1365,455,105,15,1},
    {1,16,120,560,1820,4368,8008,11440,12870,11440,8008,4368,1820,560,120,16,1},
};

static inline float st2094_intercept(uint8_t N, float Kx, float Ky)
{
    if (Kx <= 0 || Ky >= 1)
        return 1.0f / N;

    const float slope = Ky / Kx * (1 - Kx) / (1 - Ky);
    return fminf(slope / N, 1.0f);
}

static void st2094_40(float *lut, const struct pl_tone_map_params *params)
{
    const float D = params->output_max;

    // Allocate space for the adjusted bezier control points, plus endpoints
    float P[17], Kx, Ky, T;
    uint8_t N;

    if (params->hdr.ootf.num_anchors) {

        // Use bezier curve from metadata
        Kx = PL_CLAMP(params->hdr.ootf.knee_x, 0, 1);
        Ky = PL_CLAMP(params->hdr.ootf.knee_y, 0, 1);
        T = PL_CLAMP(params->hdr.ootf.target_luma, params->input_min, params->input_max);
        N = (uint8_t)(params->hdr.ootf.num_anchors + 1);
        pl_assert(N < PL_ARRAY_SIZE(P));
        memcpy(P + 1, params->hdr.ootf.anchors, (N - 1) * sizeof(*P));
        P[0] = 0.0f;
        P[N] = 1.0f;

    } else {

        // Missing metadata, default to simple brightness matching
        float src_knee, dst_knee;
        st2094_pick_knee(&src_knee, &dst_knee, params);
        Kx = src_knee / params->input_max;
        Ky = dst_knee / params->output_max;

        // Solve spline to match slope at knee intercept
        const float slope = Ky / Kx * (1 - Kx) / (1 - Ky);
        N = (uint8_t)PL_CLAMP(ceilf(slope), 2, PL_ARRAY_SIZE(P) - 1);
        P[0] = 0.0f;
        P[1] = st2094_intercept(N, Kx, Ky);
        for (int i = 2; i <= N; i++)
            P[i] = 1.0f;
        T = D;

    }

    if (D < T) {

        // Output display darker than OOTF target, make brighter
        const float Dmin = 0.0f, u = fmaxf(0.0f, (D - Dmin) / (T - Dmin));

        // Scale down the knee point to make more room for the OOTF
        Kx *= u;
        Ky *= u;

        // Make the slope of the knee more closely approximate a clip(),
        // constrained to avoid exploding P[1]
        const float beta = N * Kx / (1 - Kx);
        const float Kxy = fminf(Kx * params->input_max / D, beta / (beta + 1));
        Ky = PL_MIX(Kxy, Ky, u);

        for (int p = 2; p <= N; p++)
            P[p] = PL_MIX(1.0f, P[p], u);

        // Make the OOTF intercept linear as D -> Dmin
        P[1] = PL_MIX(st2094_intercept(N, Kx, Ky), P[1], u);

    } else if (D > T) {

        // Output display brighter than OOTF target, make more linear
        pl_assert(params->input_max > T);
        const float w = powf(1 - (D - T) / (params->input_max - T), 1.4f);

        // Constrain the slope of the input knee to prevent it from
        // exploding and making the picture way too bright
        Ky *= T / D;

        // Make the slope of the knee more linear by solving for f(Kx) = Kx
        float Kxy = Kx * D / params->input_max;
        Ky = PL_MIX(Kxy, Ky, w);

        for (int p = 2; p < N; p++) {
            float anchor_lin = (float) p / N;
            P[p] = PL_MIX(anchor_lin, P[p], w);
        }

        // Make the OOTF intercept linear as D -> input_max
        P[1] = PL_MIX(st2094_intercept(N, Kx, Ky), P[1], w);

    }

    pl_assert(Kx >= 0 && Kx <= 1);
    pl_assert(Ky >= 0 && Ky <= 1);

    FOREACH_LUT(lut, x) {
        x = bt1886_oetf(x, params->input_min, params->input_max);
        x = bt1886_eotf(x, 0.0f, 1.0f);

        if (x <= Kx && Kx) {
            // Linear section
            x *= Ky / Kx;
        } else {
            // Bezier section
            const float t = (x - Kx) / (1 - Kx);

            x = 0; // Bn
            for (uint8_t p = 0; p <= N; p++)
                x += binom[N][p] * powf(t, (float)p) * powf(1 - t, (float)(N - p)) * P[p];

            x = Ky + (1 - Ky) * x;
        }

        x = bt1886_oetf(x, 0.0f, 1.0f);
        x = bt1886_eotf(x, params->output_min, params->output_max);
    }
}

static void spline(float *lut, const struct pl_tone_map_params *params)
{
    float src_pivot, dst_pivot;
    st2094_pick_knee(&src_pivot, &dst_pivot, params);

    // Solve for linear knee (Pk = 0)
    float slope = (dst_pivot - params->output_min) /
                  (src_pivot - params->input_min);

    // Tune the slope at the knee point slightly: raise it to a user-provided
    // gamma exponent, multiplied by an extra tuning coefficient designed to
    // make the slope closer to 1.0 when the difference in peaks is low, and
    // closer to linear when the difference between peaks is high.
    float ratio = params->input_max / params->output_max - 1.0f;
    ratio = fclampf(params->constants.slope_tuning * ratio,
                    params->constants.slope_offset,
                    1.0f + params->constants.slope_offset);
    slope = powf(slope, (1.0f - params->constants.spline_contrast) * ratio);

    // Normalize everything the pivot to make the math easier
    const float in_min = params->input_min - src_pivot;
    const float in_max = params->input_max - src_pivot;
    const float out_min = params->output_min - dst_pivot;
    const float out_max = params->output_max - dst_pivot;

    // Solve hyperbolic segments P (toe) and Q (shoulder) for:
    //  P(0.0) = 0.0, P'(0.0) = slope, P(in_min) = out_min
    //  Q(0.0) = 0.0, Q'(0.0) = slope, Q(in_max) = out_max
    // with P(x) = slope * x / (1 + Pk * x), and Q analogously.
    const float Pk = (slope * in_min - out_min) / (out_min * in_min);
    const float Qk = (slope * in_max - out_max) / (out_max * in_max);

    FOREACH_LUT(lut, x) {
        x -= src_pivot;
        x = slope * x / (1.0f + (x > 0 ? Qk : Pk) * x);
        x += dst_pivot;
    }
}


void frzz_hdr_make_tone_curve(HdrColor* color, const HdrBrightness* brightness)
{
    const float display_peak = 203.0f;
    const float peak = brightness->max_nits >= 5.0f ? brightness->max_nits : 1000.0f;
    const float source_max = nits_to_pq(fclampf(peak, display_peak, 10000.0f));
    const float source_min = brightness->min_nits > 0.0f && brightness->min_nits < peak
        ? nits_to_pq(brightness->min_nits) : 0.0f;
    const float display_pq = nits_to_pq(display_peak);
    const float source_avg = brightness->avg_nits > 0.0f ? nits_to_pq(brightness->avg_nits) : 0.0f;
    float target_pq = display_pq;
    float exposure = 1.0f;
    if (source_avg > 0.0f && source_max > display_pq + 1e-4f) {
        const float average = source_avg - 0.09f;
        const float candidate = fclampf(0.5f * (display_pq + average) +
                                        hypotf(0.5f * (display_pq - average), 0.04f),
                                        display_pq, source_max);
        if (candidate > display_pq + 1e-4f && candidate < source_max - 1e-4f) {
            target_pq = candidate;
            exposure = display_peak / frzz_hdr_pq_to_nits(target_pq);
        }
    }

    const enum Scaling scale = brightness->hdr10plus ? PL_HDR_NITS : PL_HDR_PQ;
    struct pl_tone_map_params params = {
        .input_scaling = scale,
        .output_scaling = scale,
        .lut_size = FRZZ_HDR_TONE_SAMPLES,
        .input_min = pl_hdr_rescale(PL_HDR_PQ, scale, source_min),
        .input_max = pl_hdr_rescale(PL_HDR_PQ, scale, source_max),
        .input_avg = pl_hdr_rescale(PL_HDR_PQ, scale, source_avg),
        .output_min = 0.0f,
        .output_max = pl_hdr_rescale(PL_HDR_PQ, scale, target_pq),
        .constants = {0.4f, 0.1f, 0.8f, 0.4f, 1.5f, 0.2f, 0.5f},
    };
    if (brightness->anchor_count <= PL_ARRAY_SIZE(params.hdr.ootf.anchors)) {
        params.hdr.ootf.num_anchors = brightness->anchor_count;
        params.hdr.ootf.target_luma = brightness->target_nits;
        params.hdr.ootf.knee_x = brightness->knee_x;
        params.hdr.ootf.knee_y = brightness->knee_y;
        memcpy(params.hdr.ootf.anchors, brightness->anchors,
               brightness->anchor_count * sizeof(float));
    }

    for (size_t i = 0; i < params.lut_size; ++i) {
        const float pq = PL_MIX(source_min, source_max, (float)i / (params.lut_size - 1));
        color->tone_lut[i] = pl_hdr_rescale(PL_HDR_PQ, scale, pq);
    }
    if (params.input_max > params.input_min && source_max > target_pq + 1e-6f) {
        if (brightness->hdr10plus)
            st2094_40(color->tone_lut, &params);
        else
            spline(color->tone_lut, &params);
    }
    for (size_t i = 0; i < params.lut_size; ++i) {
        const float value = fclampf(color->tone_lut[i], params.output_min, params.output_max);
        color->tone_lut[i] = pl_hdr_rescale(scale, PL_HDR_PQ, value);
    }
    color->tone_limits[0] = source_min;
    color->tone_limits[1] = 1.0f / fmaxf(source_max - source_min, 1e-6f);
    color->tone_limits[2] = exposure * (10000.0f / display_peak);
}

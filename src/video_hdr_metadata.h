#pragma once

#include "video_hdr_color.h"

#include <math.h>
#include <string.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>

// AVDOVIMetadata uses offsets because FFmpeg can extend its private allocation
// without changing the ABI of the AVFrame that carries it.
static inline const void* hdr_rpu_section(const AVFrameSideData* data, size_t offset, size_t size)
{
    const size_t prefix = offsetof(AVDOVIMetadata, color_offset) + sizeof(size_t);
    if (!data || !data->data || data->size < prefix || offset < prefix ||
        offset > data->size || size > data->size - offset)
        return NULL;
    return data->data + offset;
}

static inline bool hdr_dovi_curve(HdrColor* color, unsigned component,
                                  const AVDOVIReshapingCurve* curve, float pivots, float coefficients)
{
    const unsigned count = curve->num_pivots;
    if (count < 2 || count > FRZZ_HDR_DOVI_PIVOTS)
        return false;
    color->pivot_limits[component][0] = pivots * curve->pivots[0];
    color->pivot_limits[component][1] = pivots * curve->pivots[count - 1];
    color->pivot_limits[component][2] = (float)(count - 1);
    for (unsigned i = 1; i < count; ++i) {
        if (curve->pivots[i] <= curve->pivots[i - 1])
            return false;
        color->pivots[component][i / 4][i % 4] = pivots * curve->pivots[i];
    }
    for (unsigned piece = 0; piece < count - 1; ++piece) {
        float* values = color->coefficients[component][piece];
        if (curve->mapping_idc[piece] == AV_DOVI_MAPPING_POLYNOMIAL) {
            if (curve->poly_order[piece] > 2)
                return false;
            for (unsigned exponent = 0; exponent <= curve->poly_order[piece]; ++exponent)
                values[exponent] = coefficients * (float)curve->poly_coef[piece][exponent];
        } else if (curve->mapping_idc[piece] == AV_DOVI_MAPPING_MMR) {
            const unsigned order = curve->mmr_order[piece];
            if (!order || order > FRZZ_HDR_DOVI_MMR_ORDER)
                return false;
            values[0] = coefficients * (float)curve->mmr_constant[piece];
            values[3] = (float)order;
            for (unsigned i = 0; i < order; ++i)
                for (unsigned j = 0; j < 7; ++j)
                    color->mmr[component][piece][i][j / 4][j % 4] =
                        coefficients * (float)curve->mmr_coef[piece][i][j];
        } else {
            return false;
        }
    }
    return true;
}

// Base-layer reshaping and matrix normalization adapted from libplacebo
// v7.387.0 utils/libav_internal.h and shaders/colorspace.c (LGPL-2.1-or-later).
static inline bool hdr_read_dovi(HdrColor* color, HdrBrightness* brightness, const AVFrameSideData* data)
{
    AVDOVIMetadata offsets = {0};
    const size_t prefix = offsetof(AVDOVIMetadata, color_offset) + sizeof(size_t);
    if (!data || !data->data || data->size < prefix)
        return false;
    memcpy(&offsets, data->data, prefix);
    const AVDOVIRpuDataHeader* header = hdr_rpu_section(data, offsets.header_offset,
        offsetof(AVDOVIRpuDataHeader, disable_residual_flag) + sizeof(uint8_t));
    const AVDOVIDataMapping* mapping = hdr_rpu_section(data, offsets.mapping_offset,
        sizeof(((AVDOVIDataMapping*)0)->curves) + offsetof(AVDOVIDataMapping, curves));
    const AVDOVIColorMetadata* rpu = hdr_rpu_section(data, offsets.color_offset,
        offsetof(AVDOVIColorMetadata, source_max_pq) + sizeof(uint16_t));
    if (!header || !mapping || !rpu || header->bl_bit_depth != 10 || header->coef_log2_denom > 32 ||
        !rpu->source_max_pq || rpu->source_max_pq > 4095 || rpu->source_min_pq >= rpu->source_max_pq)
        return false;

    const float pivot_scale = ldexpf(1.0f, -(int)header->bl_bit_depth);
    const float coef_scale = ldexpf(1.0f, -(int)header->coef_log2_denom);
    for (unsigned i = 0; i < 3; ++i) {
        if (!hdr_dovi_curve(color, i, &mapping->curves[i], pivot_scale, coef_scale) ||
            !rpu->ycc_to_rgb_offset[i].den)
            return false;
    }
    static const float lms_to_2020[3][3] = {
        { 3.06441879f, -2.16597676f,  0.10155818f},
        {-0.65612108f,  1.78554118f, -0.12943749f},
        { 0.01736321f, -0.04725154f,  1.03004253f},
    };
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned j = 0; j < 3; ++j) {
            if (!rpu->ycc_to_rgb_matrix[i * 3 + j].den || !rpu->rgb_to_lms_matrix[i * 3 + j].den)
                return false;
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned j = 0; j < 3; ++j) {
            const float coefficient = (float)av_q2d(rpu->ycc_to_rgb_matrix[i * 3 + j]);
            color->signal_matrix[i][j] = coefficient;
            color->signal_matrix[i][3] -= coefficient * (float)av_q2d(rpu->ycc_to_rgb_offset[j]);
            for (unsigned k = 0; k < 3; ++k)
                color->linear_matrix[i][j] += lms_to_2020[i][k] *
                    (float)av_q2d(rpu->rgb_to_lms_matrix[k * 3 + j]);
        }
    }
    brightness->min_nits = frzz_hdr_pq_to_nits(rpu->source_min_pq / 4095.0f);
    brightness->max_nits = frzz_hdr_pq_to_nits(rpu->source_max_pq / 4095.0f);
    color->tone_limits[3] = 1.0f;
    return true;
}

static inline void hdr_read_brightness(HdrBrightness* brightness, const AVFrameSideData* mastering,
                                       const AVFrameSideData* light, const AVFrameSideData* dynamic)
{
    if (mastering && mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
        const AVMasteringDisplayMetadata* md = (const AVMasteringDisplayMetadata*)mastering->data;
        if (md->has_luminance) {
            brightness->min_nits = (float)av_q2d(md->min_luminance);
            brightness->max_nits = (float)av_q2d(md->max_luminance);
        }
    }
    if (light && light->size >= sizeof(AVContentLightMetadata)) {
        const AVContentLightMetadata* cll = (const AVContentLightMetadata*)light->data;
        if (cll->MaxCLL > 0 && cll->MaxCLL <= 10000)
            brightness->max_nits = (float)cll->MaxCLL;
    }
    if (!dynamic || dynamic->size < sizeof(AVDynamicHDRPlus))
        return;
    const AVDynamicHDRPlus* hdr = (const AVDynamicHDRPlus*)dynamic->data;
    if (!hdr->num_windows || hdr->application_version >= 2)
        return;
    const AVHDRPlusColorTransformParams* window = &hdr->params[0];
    const unsigned percentiles = window->num_distribution_maxrgb_percentiles;
    const float histogram_peak = percentiles && percentiles <= 15
        ? 10000.0f * (float)av_q2d(window->distribution_maxrgb[percentiles - 1].percentile) : 0.0f;
    float max_rgb = 0.0f;
    for (unsigned c = 0; c < 3; ++c) {
        const float max_cll = 10000.0f * (float)av_q2d(window->maxscl[c]);
        max_rgb = fmaxf(max_rgb, max_cll > 0 ? max_cll : histogram_peak);
    }
    if (max_rgb >= 5.0f && max_rgb <= 10000)
        brightness->max_nits = max_rgb;
    brightness->avg_nits = 10000.0f * (float)av_q2d(window->average_maxrgb);
    if (hdr->application_version == 1 && percentiles == 9 &&
        window->distribution_maxrgb[1].percentage == 5 &&
        window->distribution_maxrgb[2].percentage == 10) {
        const float y99 = 10000.0f * (float)av_q2d(window->distribution_maxrgb[1].percentile);
        if (y99 > 0.01f && max_rgb > 0.0f)
            brightness->avg_nits *= y99 / max_rgb;
    }
    brightness->hdr10plus = true;
    if (window->tone_mapping_flag && window->num_bezier_curve_anchors <= 15) {
        brightness->target_nits = (float)av_q2d(hdr->targeted_system_display_maximum_luminance);
        brightness->knee_x = (float)av_q2d(window->knee_point_x);
        brightness->knee_y = (float)av_q2d(window->knee_point_y);
        brightness->anchor_count = window->num_bezier_curve_anchors;
        for (unsigned i = 0; i < brightness->anchor_count; ++i)
            brightness->anchors[i] = (float)av_q2d(window->bezier_curve_anchors[i]);
    }
}

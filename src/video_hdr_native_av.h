#pragma once

#include "video_hdr_metadata.h"
#include <libavutil/version.h>

static AVFrameSideData* (*hdr_get_side_data)(const AVFrame*, enum AVFrameSideDataType);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 12, 100)
static AVDOVIDmData* (*hdr_find_dovi_level)(const AVDOVIMetadata*, uint8_t);
#endif

static bool bind_hdr_metadata(void* util, void* (*lookup)(void*, const char*))
{
    hdr_get_side_data = (AVFrameSideData* (*)(const AVFrame*, enum AVFrameSideDataType))
        lookup(util, "av_frame_get_side_data");
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 12, 100)
    hdr_find_dovi_level = (AVDOVIDmData* (*)(const AVDOVIMetadata*, uint8_t))
        lookup(util, "av_dovi_find_level");
#endif
    return hdr_get_side_data != NULL;
}

static bool hdr_non_dovi_matrix(HdrColor* color, const AVFrame* source)
{
    if (source->colorspace != AVCOL_SPC_BT2020_NCL && source->colorspace != AVCOL_SPC_BT709 &&
        source->colorspace != AVCOL_SPC_UNSPECIFIED)
        return false;
    const bool bt709 = source->colorspace == AVCOL_SPC_BT709;
    const float kr = bt709 ? 0.2126f : 0.2627f;
    const float kb = bt709 ? 0.0722f : 0.0593f;
    const float kg = 1.0f - kr - kb;
    const float scale = 65535.0f / 65536.0f;
    const bool full = source->color_range == AVCOL_RANGE_JPEG;
    const float luma_bias = full ? 0.0f : (16.0f / 256.0f) / scale;
    const float luma_scale = full ? 65535.0f / (1023.0f * 64.0f) : 256.0f * scale / 219.0f;
    const float chroma_scale = full ? luma_scale : 256.0f * scale / 224.0f;
    const float chroma_bias = 0.5f / scale;
    const float ratios[3][2] = {
        {0.0f, 2.0f * (1.0f - kr)},
        {-2.0f * kb * (1.0f - kb) / kg, -2.0f * kr * (1.0f - kr) / kg},
        {2.0f * (1.0f - kb), 0.0f},
    };
    for (unsigned i = 0; i < 3; ++i) {
        color->signal_matrix[i][0] = luma_scale;
        color->signal_matrix[i][1] = chroma_scale * ratios[i][0];
        color->signal_matrix[i][2] = chroma_scale * ratios[i][1];
        color->signal_matrix[i][3] = -luma_scale * luma_bias -
            chroma_bias * (color->signal_matrix[i][1] + color->signal_matrix[i][2]);
        color->linear_matrix[i][i] = 1.0f;
    }
    return true;
}

static bool hdr_describe_color(HdrColor* color, const AVFrame* source, bool apply_rpu)
{
    // Both GPU backends serialize this adapter's preparation and rendering.
    static HdrBrightness last_brightness;
    static float last_curve[FRZZ_HDR_TONE_SAMPLES], last_limits[3];
    static bool curve_ready;
    memset(color, 0, sizeof(*color));
    HdrBrightness brightness;
    memset(&brightness, 0, sizeof(brightness));
    hdr_read_brightness(&brightness,
        hdr_get_side_data(source, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA),
        hdr_get_side_data(source, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL),
        apply_rpu ? NULL : hdr_get_side_data(source, AV_FRAME_DATA_DYNAMIC_HDR_PLUS));
    if (apply_rpu) {
        const AVFrameSideData* rpu = hdr_get_side_data(source, AV_FRAME_DATA_DOVI_METADATA);
        if (!hdr_read_dovi(color, &brightness, rpu))
            return false;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 12, 100)
        if (hdr_find_dovi_level && rpu->size >= sizeof(AVDOVIMetadata)) {
            const AVDOVIMetadata* metadata = (const AVDOVIMetadata*)rpu->data;
            // FFmpeg 7.0 lacks extension levels, so the dynamic symbol and the
            // bounds of its later allocation both have to be checked.
            if (metadata->ext_block_size >= sizeof(AVDOVIDmData) &&
                metadata->ext_block_offset <= rpu->size &&
                metadata->num_ext_blocks >= 0 &&
                (size_t)metadata->num_ext_blocks <= (rpu->size - metadata->ext_block_offset) / metadata->ext_block_size) {
                const AVDOVIDmData* level = hdr_find_dovi_level(metadata, 1);
                if (level && level->l1.max_pq > 0 && level->l1.max_pq <= 4095) {
                    brightness.max_nits = frzz_hdr_pq_to_nits(level->l1.max_pq / 4095.0f);
                    if (level->l1.avg_pq <= level->l1.max_pq)
                        brightness.avg_nits = frzz_hdr_pq_to_nits(level->l1.avg_pq / 4095.0f);
                }
            }
        }
#endif
    } else if (!hdr_non_dovi_matrix(color, source)) {
        return false;
    }
    static const float bt2020_to_709[3][4] = {
        { 1.660491f, -0.587641f, -0.072850f, 0.0f},
        {-0.124550f,  1.132900f, -0.008349f, 0.0f},
        {-0.018151f, -0.100579f,  1.118730f, 0.0f},
    };
    static const float p3_to_709[3][4] = {
        { 1.224940f, -0.224940f,  0.000000f, 0.0f},
        {-0.042057f,  1.042057f,  0.000000f, 0.0f},
        {-0.019638f, -0.078636f,  1.098274f, 0.0f},
    };
    if (apply_rpu || source->color_primaries == AVCOL_PRI_BT2020 ||
        source->color_primaries == AVCOL_PRI_UNSPECIFIED) {
        memcpy(color->gamut_matrix, bt2020_to_709, sizeof(bt2020_to_709));
    } else if (source->color_primaries == AVCOL_PRI_SMPTE432) {
        memcpy(color->gamut_matrix, p3_to_709, sizeof(p3_to_709));
    } else if (source->color_primaries == AVCOL_PRI_BT709) {
        for (unsigned i = 0; i < 3; ++i)
            color->gamut_matrix[i][i] = 1.0f;
    } else {
        return false;
    }
    if (!curve_ready || memcmp(&brightness, &last_brightness, sizeof(brightness))) {
        frzz_hdr_make_tone_curve(color, &brightness);
        memcpy(&last_brightness, &brightness, sizeof(brightness));
        memcpy(last_curve, color->tone_lut, sizeof(last_curve));
        memcpy(last_limits, color->tone_limits, sizeof(last_limits));
        curve_ready = true;
    } else {
        memcpy(color->tone_lut, last_curve, sizeof(last_curve));
        memcpy(color->tone_limits, last_limits, sizeof(last_limits));
    }
    return true;
}

static void hdr_chroma_shift(enum AVChromaLocation location, float shift[2])
{
    shift[0] = (location == AVCHROMA_LOC_UNSPECIFIED || location == AVCHROMA_LOC_LEFT ||
                location == AVCHROMA_LOC_TOPLEFT || location == AVCHROMA_LOC_BOTTOMLEFT) ? -0.5f : 0.0f;
    shift[1] = (location == AVCHROMA_LOC_TOP || location == AVCHROMA_LOC_TOPLEFT) ? -0.5f :
               (location == AVCHROMA_LOC_BOTTOM || location == AVCHROMA_LOC_BOTTOMLEFT) ? 0.5f : 0.0f;
}

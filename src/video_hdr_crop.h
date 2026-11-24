#pragma once
#include <stdbool.h>
#include <libavutil/frame.h>

struct HdrCrop {
    int left, top, width, height;
};

static inline bool hdr_crop(const AVFrame* frame, struct HdrCrop* result)
{
    // The decoder may have applied part of the crop already. Use the remaining margins.
    if (frame->width <= 0 || frame->height <= 0 ||
        ((frame->crop_left | frame->crop_right | frame->crop_top | frame->crop_bottom) & 1) ||
        frame->crop_left >= (size_t)frame->width ||
        frame->crop_top >= (size_t)frame->height ||
        frame->crop_right >= (size_t)frame->width - frame->crop_left ||
        frame->crop_bottom >= (size_t)frame->height - frame->crop_top)
        return false;
    result->left = (int)frame->crop_left;
    result->top = (int)frame->crop_top;
    result->width = frame->width - result->left - (int)frame->crop_right;
    result->height = frame->height - result->top - (int)frame->crop_bottom;
    return !(result->width & 1) && !(result->height & 1);
}

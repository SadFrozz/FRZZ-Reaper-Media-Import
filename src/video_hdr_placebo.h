#pragma once
#include <stdbool.h>
#include "video_hdr_symbols.h"

#ifdef __cplusplus
extern "C" {
#endif
struct AVFrame;
bool FRZZ_HDR_API(frzz_hdr_bind_av)(void* util);
bool FRZZ_HDR_API(frzz_hdr_render)(struct AVFrame* output,const struct AVFrame* source,
                     const unsigned input[2],const unsigned target[2],
                     const int widths[2],const int heights[2],bool apply_rpu,bool prepare_output_only,
                     unsigned prepare_dovi_profile);
#ifdef __cplusplus
}
#endif

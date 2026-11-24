#pragma once

// Numbers and operators are shared by the HLSL and GLSL implementations.
// Curve sampling and RPU reshaping follow libplacebo v7.387.0 (LGPL-2.1-or-later).
#define FRZZ_HDR_COLOR_FIELDS \
    "V4 signal_matrix[3]; V4 linear_matrix[3]; V4 gamut_matrix[3]; V4 tone_limits;\n" \
    "V4 pivot_limits[3]; V4 pivots[9]; V4 coefficients[24]; V4 mmr[144]; V4 tone_lut[64];\n"

#define FRZZ_HDR_COLOR_SHADER \
    "V3 hdr_matrix(V4 row0, V4 row1, V4 row2, V3 value) {\n" \
    "    V4 s=V4(value, 1.0);\n" \
    "    return V3(dot(row0,s), dot(row1,s), dot(row2,s));\n" \
    "}\n" \
    "float hdr_reshape(V3 sig, int c) {\n" \
    "    float value=sig[c];\n" \
    "    int piece=0;\n" \
    "    for (int p=1; p<int(pivot_limits[c].z); ++p)\n" \
    "        piece += value >= pivots[c*3+p/4][p%4] ? 1 : 0;\n" \
    "    int index=c*8+piece;\n" \
    "    V4 coef=coefficients[index];\n" \
    "    float result=coef.x;\n" \
    "    if (coef.w==0.0) {\n" \
    "        result += value*(coef.y+value*coef.z);\n" \
    "    } else {\n" \
    "        V4 cross_terms=V4(sig.x*sig.y,sig.x*sig.z,sig.y*sig.z,sig.x*sig.y*sig.z);\n" \
    "        V3 power=sig; V4 products=cross_terms;\n" \
    "        for (int order=0; order<int(coef.w); ++order) {\n" \
    "            int base=index*6+order*2;\n" \
    "            result += dot(power,mmr[base].xyz)+dot(products,V4(mmr[base].w,mmr[base+1].xyz));\n" \
    "            power*=sig; products*=cross_terms;\n" \
    "        }\n" \
    "    }\n" \
    "    return clamp(result,0.0,1.0);\n" \
    "}\n" \
    "V3 hdr_eotf(V3 x) {\n" \
    "    V3 p=pow(clamp(x,V3(0.0,0.0,0.0),V3(1.0,1.0,1.0)), V3(32.0,32.0,32.0)/2523.0);\n" \
    "    return pow(max(p-3424.0/4096.0,V3(0.0,0.0,0.0))/(2413.0/128.0-2392.0/128.0*p),\n" \
    "               V3(16384.0,16384.0,16384.0)/2610.0);\n" \
    "}\n" \
    "float hdr_oetf(float value) {\n" \
    "    float p=pow(clamp(value,0.0,1.0),2610.0/16384.0);\n" \
    "    return pow((3424.0/4096.0+2413.0/128.0*p)/(1.0+2392.0/128.0*p),2523.0/32.0);\n" \
    "}\n" \
    "float hdr_curve(float value) {\n" \
    "    float p=clamp((value-tone_limits.x)*tone_limits.y,0.0,1.0)*255.0;\n" \
    "    int lo=int(p), hi=min(lo+1,255);\n" \
    "    return BLEND(tone_lut[lo/4][lo%4],tone_lut[hi/4][hi%4],p-float(lo));\n" \
    "}\n" \
    "V3 hdr_to_sdr(V3 signal) {\n" \
    "    if (tone_limits.w>0.0) {\n" \
    "        V3 sig=signal*(65535.0/65536.0);\n" \
    "        sig=clamp(sig,V3(pivot_limits[0].x,pivot_limits[1].x,pivot_limits[2].x),\n" \
    "                     V3(pivot_limits[0].y,pivot_limits[1].y,pivot_limits[2].y));\n" \
    "        signal=V3(hdr_reshape(sig,0),hdr_reshape(sig,1),hdr_reshape(sig,2));\n" \
    "    }\n" \
    "    V3 encoded=hdr_matrix(signal_matrix[0],signal_matrix[1],signal_matrix[2],signal);\n" \
    "    V3 rgb=hdr_matrix(linear_matrix[0],linear_matrix[1],linear_matrix[2],hdr_eotf(encoded));\n" \
    "    float peak=max(max(rgb.x,rgb.y),max(rgb.z,0.0));\n" \
    "    float mapped=hdr_curve(hdr_oetf(peak));\n" \
    "    float gain=hdr_eotf(V3(mapped,mapped,mapped)).x*tone_limits.z/max(peak,1e-9);\n" \
    "    rgb=hdr_matrix(gamut_matrix[0],gamut_matrix[1],gamut_matrix[2],rgb*gain);\n" \
    "    V3 weights=V3(0.2126,0.7152,0.0722);\n" \
    "    float y=clamp(dot(rgb,weights),0.0,1.0);\n" \
    "    float low=min(rgb.x,min(rgb.y,rgb.z)), high=max(rgb.x,max(rgb.y,rgb.z));\n" \
    "    float chroma=min(1.0,min(y/max(y-low,1e-9),(1.0-y)/max(high-y,1e-9)));\n" \
    "    rgb=pow(clamp(y+chroma*(rgb-y),V3(0.0,0.0,0.0),V3(1.0,1.0,1.0)),V3(1.0,1.0,1.0)/2.4);\n" \
    "    y=dot(rgb,weights);\n" \
    "    return (V3(219.0*y,224.0*(rgb.z-y)/1.8556,224.0*(rgb.x-y)/1.5748)+V3(16.0,128.0,128.0))/255.0;\n" \
    "}\n"
